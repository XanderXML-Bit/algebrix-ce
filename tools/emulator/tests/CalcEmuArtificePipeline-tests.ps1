Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:ModulePath = (Resolve-Path (Join-Path $PSScriptRoot "..\CalcEmuArtificePipeline.psm1")).ProviderPath
if (-not (Test-Path -LiteralPath $script:ModulePath -PathType Leaf)) {
    Write-Error "CalcEmuArtificePipeline module was not found at '$script:ModulePath'."
    exit 2
}
Import-Module -Name $script:ModulePath -Force

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)][AllowNull()][object]$Expected,
        [Parameter(Mandatory = $true)][AllowNull()][object]$Actual,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual: $Actual"
    }
}

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)][string]$Needle,
        [Parameter(Mandatory = $true)][string]$Haystack,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Haystack.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "$Message`nExpected to find: $Needle`nActual text: $Haystack"
    }
}

function Invoke-TestCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $script:TestTotal++
    try {
        & $Body
        $script:TestPassed++
        Write-Host "[PASS] $Name"
    }
    catch {
        $script:TestFailed++
        Write-Host "[FAIL] $Name"
        Write-Host $_.Exception.Message
    }
}

function New-TestDirectory {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("artifice-pipeline-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $path -Force)
    $script:CreatedTempDirectories.Add($path)
    return $path
}

function New-BinaryFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

try {
    # ---------------------------------------------------------------------------
    # Normalize-CalcEmuHashValue tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Normalize-CalcEmuHashValue returns empty string for empty input" -Body {
        $result = Normalize-CalcEmuHashValue -Value ""
        Assert-Equal -Expected "" -Actual $result -Message "Empty input should return empty string."
    }

    Invoke-TestCase -Name "Normalize-CalcEmuHashValue strips 0x prefix and uppercases" -Body {
        $result = Normalize-CalcEmuHashValue -Value "0x1234abcd"
        Assert-Equal -Expected "1234ABCD" -Actual $result -Message "0x prefix should be stripped and value uppercased."
    }

    Invoke-TestCase -Name "Normalize-CalcEmuHashValue left-pads short hex to 8 digits" -Body {
        $result = Normalize-CalcEmuHashValue -Value "ff"
        Assert-Equal -Expected "000000FF" -Actual $result -Message "Short hex should be left-padded to 8 uppercase digits."
    }

    Invoke-TestCase -Name "Normalize-CalcEmuHashValue preserves exact 8-digit uppercase hex" -Body {
        $result = Normalize-CalcEmuHashValue -Value "B9B5D5E0"
        Assert-Equal -Expected "B9B5D5E0" -Actual $result -Message "Exact 8-digit uppercase hex should pass through unchanged."
    }

    Invoke-TestCase -Name "Normalize-CalcEmuHashValue returns empty for whitespace-only input" -Body {
        $result = Normalize-CalcEmuHashValue -Value "   "
        Assert-Equal -Expected "" -Actual $result -Message "Whitespace-only input should return empty string."
    }

    # ---------------------------------------------------------------------------
    # Get-CalcEmuAlphaLaunchSequence tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Get-CalcEmuAlphaLaunchSequence produces two entries per character" -Body {
        $sequence = @(Get-CalcEmuAlphaLaunchSequence -ProgramName "A")
        Assert-Equal -Expected 2 -Actual $sequence.Count -Message "Single-character program name should produce 2 key entries."
        Assert-Equal -Expected "key|alpha" -Actual $sequence[0] -Message "First entry should be key|alpha."
        Assert-Equal -Expected "key|math" -Actual $sequence[1] -Message "Second entry should be the alpha-mapped key for A (math)."
    }

    Invoke-TestCase -Name "Get-CalcEmuAlphaLaunchSequence produces 14 entries for CALCENG" -Body {
        # CALCENG: C=prgm A=math L=) C=prgm E=sin N=log G=tan  (7 chars x 2 keys = 14)
        $sequence = @(Get-CalcEmuAlphaLaunchSequence -ProgramName "CALCENG")
        Assert-Equal -Expected 14 -Actual $sequence.Count -Message "CALCENG has 7 characters, each producing 2 key entries = 14 total."
        Assert-Equal -Expected "key|alpha" -Actual $sequence[0] -Message "First entry should be key|alpha."
        Assert-Equal -Expected "key|prgm" -Actual $sequence[1] -Message "C maps to key|prgm."
        Assert-Equal -Expected "key|alpha" -Actual $sequence[2] -Message "Third entry should be key|alpha."
        Assert-Equal -Expected "key|math" -Actual $sequence[3] -Message "A maps to key|math."
        Assert-Equal -Expected "key|alpha" -Actual $sequence[10] -Message "N prefix should be key|alpha (indices 10-11)."
        Assert-Equal -Expected "key|log" -Actual $sequence[11] -Message "N maps to key|log (printed keycap)."
        Assert-Equal -Expected "key|alpha" -Actual $sequence[12] -Message "G prefix should be key|alpha (indices 12-13)."
        Assert-Equal -Expected "key|tan" -Actual $sequence[13] -Message "G maps to key|tan."
    }

    Invoke-TestCase -Name "Get-CalcEmuAlphaLaunchSequence throws for unsupported character" -Body {
        $threw = $false
        try {
            Get-CalcEmuAlphaLaunchSequence -ProgramName "A1"
        }
        catch {
            $threw = $true
        }
        Assert-True -Condition $threw -Message "Program name with digit should throw because digits are not in the alpha key map."
    }

    # ---------------------------------------------------------------------------
    # Get-CalcEmuArtificeScreenDefinitions tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Get-CalcEmuArtificeScreenDefinitions returns exactly six screens" -Body {
        $screens = @(Get-CalcEmuArtificeScreenDefinitions)
        Assert-Equal -Expected 6 -Actual $screens.Count -Message "Exactly six screen definitions should be returned."
    }

    Invoke-TestCase -Name "Get-CalcEmuArtificeScreenDefinitions has expected step names in order" -Body {
        $screens = @(Get-CalcEmuArtificeScreenDefinitions)
        Assert-Equal -Expected "artifice_shell_loaded" -Actual ([string]$screens[0].step) -Message "First step should be artifice_shell_loaded."
        Assert-Equal -Expected "artifice_quit_transition" -Actual ([string]$screens[1].step) -Message "Second step should be artifice_quit_transition."
        Assert-Equal -Expected "ti_os_program_menu_open" -Actual ([string]$screens[2].step) -Message "Third step should be ti_os_program_menu_open."
        Assert-Equal -Expected "calcengine_launch_menu" -Actual ([string]$screens[3].step) -Message "Fourth step should be calcengine_launch_menu."
        Assert-Equal -Expected "calcengine_result_2plus3" -Actual ([string]$screens[4].step) -Message "Fifth step should be calcengine_result_2plus3."
        Assert-Equal -Expected "calcengine_menu_open" -Actual ([string]$screens[5].step) -Message "Sixth step should be calcengine_menu_open."
    }

    Invoke-TestCase -Name "Get-CalcEmuArtificeScreenDefinitions each screen has non-empty description" -Body {
        foreach ($screen in @(Get-CalcEmuArtificeScreenDefinitions)) {
            Assert-True -Condition (-not [string]::IsNullOrWhiteSpace([string]$screen.description)) `
                -Message ("Screen '{0}' must have a non-empty description." -f [string]$screen.step)
        }
    }

    # ---------------------------------------------------------------------------
    # New-CalcEmuArtificeTemplateObject tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "New-CalcEmuArtificeTemplateObject target name is A and is not ASM" -Body {
        $template = New-CalcEmuArtificeTemplateObject
        Assert-Equal -Expected "A" -Actual ([string]$template.target.name) -Message "arTIfiCE launches as program A."
        Assert-Equal -Expected $false -Actual ([bool]$template.target.isASM) -Message "arTIfiCE v2 is a TI-Basic program, not ASM."
    }

    Invoke-TestCase -Name "New-CalcEmuArtificeTemplateObject hashes object has six named entries" -Body {
        $template = New-CalcEmuArtificeTemplateObject
        # Use .Keys (not PSObject.Properties) because hashes is an ordered hashtable.
        $hashKeys = @($template.hashes.Keys)
        Assert-Equal -Expected 6 -Actual $hashKeys.Count -Message "Template should define exactly six hash entries."
        Assert-True -Condition ($hashKeys -contains "artifice_shell_loaded") -Message "Template must include artifice_shell_loaded hash."
        Assert-True -Condition ($hashKeys -contains "artifice_quit_transition") -Message "Template must include artifice_quit_transition hash."
        Assert-True -Condition ($hashKeys -contains "ti_os_program_menu_open") -Message "Template must include ti_os_program_menu_open hash."
        Assert-True -Condition ($hashKeys -contains "calcengine_launch_menu") -Message "Template must include calcengine_launch_menu hash."
        Assert-True -Condition ($hashKeys -contains "calcengine_result_2plus3") -Message "Template must include calcengine_result_2plus3 hash."
        Assert-True -Condition ($hashKeys -contains "calcengine_menu_open") -Message "Template must include calcengine_menu_open hash."
    }

    Invoke-TestCase -Name "New-CalcEmuArtificeTemplateObject sequence contains all six hash steps" -Body {
        $template = New-CalcEmuArtificeTemplateObject
        $sequenceText = (@($template.sequence) -join "`n")
        Assert-Contains -Needle "hash|artifice_shell_loaded" -Haystack $sequenceText -Message "Sequence must hash the arTIfiCE shell screen."
        Assert-Contains -Needle "key|clear" -Haystack $sequenceText -Message "Sequence must send CLEAR before MODE when leaving the arTIfiCE shell."
        Assert-Contains -Needle "hash|artifice_quit_transition" -Haystack $sequenceText -Message "Sequence must hash the post-shell quit transition."
        Assert-Contains -Needle "hash|ti_os_program_menu_open" -Haystack $sequenceText -Message "Sequence must hash the TI-OS PRGM menu before launching CalcEngine."
        Assert-Contains -Needle "hash|calcengine_launch_menu" -Haystack $sequenceText -Message "Sequence must hash the CalcEngine launch menu."
        Assert-Contains -Needle "hash|calcengine_result_2plus3" -Haystack $sequenceText -Message "Sequence must hash the 2+3 result screen."
        Assert-Contains -Needle "hash|calcengine_menu_open" -Haystack $sequenceText -Message "Sequence must hash the CalcEngine menu-open screen."
    }

    Invoke-TestCase -Name "New-CalcEmuArtificeTemplateObject uses expected hash when provided" -Body {
        $template = New-CalcEmuArtificeTemplateObject -ExpectedHashes @{
            artifice_shell_loaded = "0xAABBCCDD"
            artifice_quit_transition = "01020304"
            ti_os_program_menu_open = "55667788"
            calcengine_result_2plus3 = "11223344"
        }
        $shellExpected = @($template.hashes.artifice_shell_loaded.expected_CRCs)[0]
        $exitExpected = @($template.hashes.artifice_quit_transition.expected_CRCs)[0]
        $prgmExpected = @($template.hashes.ti_os_program_menu_open.expected_CRCs)[0]
        $resultExpected = @($template.hashes.calcengine_result_2plus3.expected_CRCs)[0]
        $launchExpected = @($template.hashes.calcengine_launch_menu.expected_CRCs)[0]
        Assert-Equal -Expected "AABBCCDD" -Actual $shellExpected -Message "Shell hash should be normalised and stored."
        Assert-Equal -Expected "01020304" -Actual $exitExpected -Message "Exit-to-home hash should be normalised and stored."
        Assert-Equal -Expected "55667788" -Actual $prgmExpected -Message "PRGM-menu hash should be stored."
        Assert-Equal -Expected "11223344" -Actual $resultExpected -Message "Result hash should be stored as-is."
        Assert-Equal -Expected "00000000" -Actual $launchExpected -Message "Unspecified hashes should default to 00000000 sentinel."
    }

    # ---------------------------------------------------------------------------
    # Find-CalcEmuArtificeCandidates tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Find-CalcEmuArtificeCandidates returns matching files from search root" -Body {
        $tempDir = New-TestDirectory
        New-BinaryFile -Path (Join-Path $tempDir "arTIfiCE_v2.8xp") -SizeBytes 1024
        New-BinaryFile -Path (Join-Path $tempDir "CALCENG.8xp") -SizeBytes 8192

        $candidates = @(Find-CalcEmuArtificeCandidates -SearchRoots @($tempDir))
        $existing = @($candidates | Where-Object { [bool]$_.exists })
        Assert-Equal -Expected 1 -Actual $existing.Count -Message "Only the arTIfiCE-named file should match the scan filter."
        Assert-Contains -Needle "arTIfiCE_v2.8xp" -Haystack ([string]$existing[0].path) -Message "The matching candidate should be the arTIfiCE-named file."
    }

    Invoke-TestCase -Name "Find-CalcEmuArtificeCandidates prioritises explicit path over scan" -Body {
        $tempDir = New-TestDirectory
        $scanFile = Join-Path $tempDir "arTIfiCE_v2.1.8xp"
        $explicitFile = Join-Path $tempDir "explicit-arTIfiCE.8xp"
        New-BinaryFile -Path $scanFile -SizeBytes 1024
        New-BinaryFile -Path $explicitFile -SizeBytes 4096

        $candidates = @(Find-CalcEmuArtificeCandidates -ExplicitPath $explicitFile -SearchRoots @($tempDir))
        # Use ForEach-Object to extract source from ordered hashtables (not PSObject.Properties).
        $sources = @($candidates | ForEach-Object { [string]$_.source })
        Assert-True -Condition ($sources -contains "explicit") -Message "Explicit path candidate must be present."
        Assert-True -Condition ($sources -contains "auto_scan") -Message "Scan candidate must also be present."
        $firstExisting = @($candidates | Where-Object { [bool]$_.exists })[0]
        Assert-Equal -Expected "explicit" -Actual ([string]$firstExisting.source) -Message "Explicit path candidate should appear first."
    }

    Invoke-TestCase -Name "Find-CalcEmuArtificeCandidates returns no matches when no artifice-named files present" -Body {
        $tempDir = New-TestDirectory
        New-BinaryFile -Path (Join-Path $tempDir "CALCENG.8xp") -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $tempDir "libload.8xv") -SizeBytes 512

        $candidates = @(Find-CalcEmuArtificeCandidates -SearchRoots @($tempDir))
        $existing = @($candidates | Where-Object { [bool]$_.exists })
        Assert-Equal -Expected 0 -Actual $existing.Count -Message "No arTIfiCE-named files means no existing candidates."
    }

    # ---------------------------------------------------------------------------
    # Convert-CalcEmuHashMapToArtificeScreens tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Convert-CalcEmuHashMapToArtificeScreens maps all six screens" -Body {
        $screens = @(Convert-CalcEmuHashMapToArtificeScreens -Hashes @{
            artifice_shell_loaded = "AABBCCDD"
            artifice_quit_transition = "01020304"
            ti_os_program_menu_open = "11223344"
            calcengine_launch_menu = "55667788"
            calcengine_result_2plus3 = "99AABBCC"
            calcengine_menu_open = "DDEEFF00"
        })
        Assert-Equal -Expected 6 -Actual $screens.Count -Message "Exactly six screens should be returned."
        Assert-Equal -Expected "AABBCCDD" -Actual ([string]$screens[0].hash) -Message "First screen hash should be normalised."
        Assert-Equal -Expected "01020304" -Actual ([string]$screens[1].hash) -Message "Second screen hash should be normalised."
        Assert-Equal -Expected "DDEEFF00" -Actual ([string]$screens[5].hash) -Message "Sixth screen hash should be normalised."
    }

    Invoke-TestCase -Name "Convert-CalcEmuHashMapToArtificeScreens records empty hash for missing keys" -Body {
        $screens = @(Convert-CalcEmuHashMapToArtificeScreens -Hashes @{
            artifice_shell_loaded = "AABBCCDD"
        })
        Assert-Equal -Expected 6 -Actual $screens.Count -Message "All six screens should still be returned even with partial hash map."
        Assert-Equal -Expected "AABBCCDD" -Actual ([string]$screens[0].hash) -Message "Provided hash should be stored."
        Assert-Equal -Expected "" -Actual ([string]$screens[1].hash) -Message "Missing hash should be empty string."
    }

    # ---------------------------------------------------------------------------
    # Stage-CalcEmuArtificeDownloadsDirectory tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Stage-CalcEmuArtificeDownloadsDirectory copies CalcEngine files and renames arTIfiCE to A.8xp" -Body {
        $tempDir = New-TestDirectory
        $sourceDir = Join-Path $tempDir "source"
        $destDir = Join-Path $tempDir "staging"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.8xp"
        [void](New-Item -ItemType Directory -Path $sourceDir -Force)
        New-BinaryFile -Path (Join-Path $sourceDir "CALCENG.8xp") -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $sourceDir "libload.8xv") -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096

        $result = Stage-CalcEmuArtificeDownloadsDirectory `
            -SourceDownloadsDir $sourceDir `
            -ArtificeSourcePath $artificePath `
            -DestinationDir $destDir

        Assert-True -Condition (Test-Path -LiteralPath $destDir -PathType Container) -Message "Destination directory should be created."
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $destDir "A.8xp") -PathType Leaf) -Message "arTIfiCE should be renamed to A.8xp in staging."
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $destDir "CALCENG.8xp") -PathType Leaf) -Message "CALCENG.8xp should be copied to staging."
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $destDir "libload.8xv") -PathType Leaf) -Message "libload.8xv should be copied to staging."
        Assert-Equal -Expected 3 -Actual ([int]$result.copied_file_count) -Message "Two CalcEngine files plus A.8xp = three copied files."
        Assert-True -Condition (-not [string]::IsNullOrWhiteSpace([string]$result.staged_artifice_path)) -Message "staged_artifice_path should be set."
    }

    # ---------------------------------------------------------------------------
    # Compare-CalcEmuArtificeCaptureToBaseline tests
    # ---------------------------------------------------------------------------

    Invoke-TestCase -Name "Compare-CalcEmuArtificeCaptureToBaseline returns BASELINE_MISSING when baseline is null" -Body {
        $capturedScreens = @(Get-CalcEmuArtificeScreenDefinitions | ForEach-Object {
            [ordered]@{ step = [string]$_.step; description = [string]$_.description; hash = "AABBCCDD" }
        })
        $comparison = Compare-CalcEmuArtificeCaptureToBaseline -CapturedScreens $capturedScreens -Baseline $null

        Assert-Equal -Expected "BASELINE_MISSING" -Actual ([string]$comparison.overall_status) -Message "Null baseline should produce BASELINE_MISSING overall status."
        Assert-Equal -Expected 0 -Actual ([int]$comparison.pass_count) -Message "Pass count must be zero when baseline is missing."
        Assert-Equal -Expected 0 -Actual ([int]$comparison.fail_count) -Message "Fail count must be zero when baseline is missing (all are baseline_missing)."
        Assert-Equal -Expected 6 -Actual ([int]$comparison.baseline_missing_count) -Message "All six screens should be baseline_missing."
    }

    Invoke-TestCase -Name "Compare-CalcEmuArtificeCaptureToBaseline reports PASS when all hashes match" -Body {
        $capturedScreens = @(Get-CalcEmuArtificeScreenDefinitions | ForEach-Object {
            [ordered]@{ step = [string]$_.step; description = [string]$_.description; hash = "AABBCCDD" }
        })
        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(Get-CalcEmuArtificeScreenDefinitions | ForEach-Object {
                [ordered]@{ step = [string]$_.step; description = [string]$_.description; hash = "AABBCCDD" }
            })
        }
        $comparison = Compare-CalcEmuArtificeCaptureToBaseline -CapturedScreens $capturedScreens -Baseline $baseline

        Assert-Equal -Expected "PASS" -Actual ([string]$comparison.overall_status) -Message "All matching hashes should produce PASS."
        Assert-Equal -Expected 6 -Actual ([int]$comparison.pass_count) -Message "All six screens should pass."
        Assert-Equal -Expected 0 -Actual ([int]$comparison.fail_count) -Message "No screens should fail."
    }

    Invoke-TestCase -Name "Compare-CalcEmuArtificeCaptureToBaseline reports FAIL for mismatched hash" -Body {
        $capturedScreens = @(
            [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
            [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAAA" },
            [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBBB" },
            [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
            [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
            [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
        )
        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAAA" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBBB" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "DEADBEEF" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $comparison = Compare-CalcEmuArtificeCaptureToBaseline -CapturedScreens $capturedScreens -Baseline $baseline

        Assert-Equal -Expected "FAIL" -Actual ([string]$comparison.overall_status) -Message "One mismatch should produce FAIL."
        Assert-Equal -Expected 5 -Actual ([int]$comparison.pass_count) -Message "Five screens should pass."
        Assert-Equal -Expected 1 -Actual ([int]$comparison.fail_count) -Message "One screen should fail."
        Assert-Equal -Expected "FAIL" -Actual ([string]$comparison.results[3].status) -Message "calcengine_launch_menu should be the failing step."
        Assert-Equal -Expected "DEADBEEF" -Actual ([string]$comparison.results[3].expected_hash) -Message "Expected hash should come from the baseline."
        Assert-Equal -Expected "22222222" -Actual ([string]$comparison.results[3].actual_hash) -Message "Actual hash should come from the capture."
    }
}
finally {
    foreach ($directory in $script:CreatedTempDirectories) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "Total tests: $script:TestTotal"
Write-Host "Passed: $script:TestPassed"
Write-Host "Failed: $script:TestFailed"

if ($script:TestFailed -gt 0) {
    exit 1
}

exit 0
