Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\record-artifice-baseline.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "Baseline recorder was not found at '$script:RunnerPath'."
    exit 2
}

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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("record-artifice-baseline-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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

function New-MinimalManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$DownloadsDir,
        [Parameter(Mandatory = $true)][string]$ProgramName,
        [Parameter(Mandatory = $true)][string]$AppVarName
    )

    $pipelineSupportPath = Join-Path $script:RepoRoot "tools\emulator\CalcEmuPipelineSupport.psm1"
    Import-Module -Name $pipelineSupportPath -Force

    $programBytes = [System.IO.File]::ReadAllBytes((Join-Path $DownloadsDir $ProgramName))
    $appVarBytes = [System.IO.File]::ReadAllBytes((Join-Path $DownloadsDir $AppVarName))

    $manifest = [ordered]@{
        schema_version = 1
        name = "Test manifest"
        version = "1.0.0"
        modules = @(
            [ordered]@{
                name = $ProgramName
                size_bytes = $programBytes.Length
                crc32 = (Get-CalcEmuCrc32Hex -Data $programBytes)
            }
        )
        runtime_dependencies = @(
            [ordered]@{
                name = $AppVarName
                size_bytes = $appVarBytes.Length
                crc32 = (Get-CalcEmuCrc32Hex -Data $appVarBytes)
            }
        )
        readme = [ordered]@{
            name = "README.md"
        }
    }

    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-FakeComprehensiveRunner {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][hashtable]$Hashes
    )

    $content = @"
param(
    [string]`$RomPath,
    [string]`$AutotesterPath,
    [string]`$Mode,
    [string]`$DownloadsDir,
    [string]`$TemplatePath,
    [string]`$GeneratedPath,
    [string]`$BaselinedPath,
    [string]`$CrcCatalogJsonPath,
    [string]`$CrcCatalogMarkdownPath,
    [string]`$SummaryPath
)

`$config = Get-Content -LiteralPath `$TemplatePath -Raw | ConvertFrom-Json
`$config.rom = `$RomPath
`$config.transfer_files = @(
    Get-ChildItem -LiteralPath `$DownloadsDir -File |
    Sort-Object Name |
    Select-Object -ExpandProperty FullName
)
`$config.hashes.artifice_shell_loaded.expected_CRCs = @('$($Hashes.artifice_shell_loaded)')
`$config.hashes.artifice_quit_transition.expected_CRCs = @('$($Hashes.artifice_quit_transition)')
`$config.hashes.ti_os_program_menu_open.expected_CRCs = @('$($Hashes.ti_os_program_menu_open)')
`$config.hashes.calcengine_launch_menu.expected_CRCs = @('$($Hashes.calcengine_launch_menu)')
`$config.hashes.calcengine_result_2plus3.expected_CRCs = @('$($Hashes.calcengine_result_2plus3)')
`$config.hashes.calcengine_menu_open.expected_CRCs = @('$($Hashes.calcengine_menu_open)')
`$config | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$BaselinedPath -Encoding UTF8

`$summary = [ordered]@{
    schema_version = 1
    mode = `$Mode
    success = `$true
    exit_code = 0
    baseline_stage = [ordered]@{
        generated_path = `$GeneratedPath
        baselined_path = `$BaselinedPath
        observed_crcs = [ordered]@{
            artifice_shell_loaded = '$($Hashes.artifice_shell_loaded)'
            artifice_quit_transition = '$($Hashes.artifice_quit_transition)'
            ti_os_program_menu_open = '$($Hashes.ti_os_program_menu_open)'
            calcengine_launch_menu = '$($Hashes.calcengine_launch_menu)'
            calcengine_result_2plus3 = '$($Hashes.calcengine_result_2plus3)'
            calcengine_menu_open = '$($Hashes.calcengine_menu_open)'
        }
        applied_count = 6
        missing_count = 0
        success = `$true
    }
}
`$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8
Write-Host 'fake record baseline runner ok'
exit 0
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function Invoke-Runner {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:RunnerPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE

    return [ordered]@{
        exit_code = [int]$exitCode
        output_text = (@($captured | ForEach-Object { [string]$_ }) -join "`n")
    }
}

try {
    Invoke-TestCase -Name "Recorder writes baseline JSON with six captured screens" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Hashes @{
            artifice_shell_loaded = "11111111"
            artifice_quit_transition = "AAAAAAA1"
            ti_os_program_menu_open = "BBBBBBB2"
            calcengine_launch_menu = "22222222"
            calcengine_result_2plus3 = "33333333"
            calcengine_menu_open = "44444444"
        }

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Recorder should succeed when capture succeeds."
        Assert-True -Condition (Test-Path -LiteralPath $baselinePath -PathType Leaf) -Message "Recorder should write the baseline file."

        $baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
        Assert-Equal -Expected 6 -Actual ([int]$baseline.screens.Count) -Message "Baseline should contain six screens."
        Assert-Equal -Expected "11111111" -Actual ([string]$baseline.screens[0].hash) -Message "Shell hash should be recorded."
        Assert-Equal -Expected "BBBBBBB2" -Actual ([string]$baseline.screens[2].hash) -Message "PRGM-menu hash should be recorded."
        Assert-Equal -Expected "44444444" -Actual ([string]$baseline.screens[5].hash) -Message "Menu hash should be recorded."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should record success."
        Assert-Equal -Expected $true -Actual ([bool]$summary.overwrite_confirmed) -Message "Recorder should mark overwrite confirmation for a new baseline."
    }

    Invoke-TestCase -Name "Recorder shows diff and overwrites existing baseline when forced" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Hashes @{
            artifice_shell_loaded = "AAAA1111"
            artifice_quit_transition = "AAAATTTT"
            ti_os_program_menu_open = "PPPPMMMM"
            calcengine_launch_menu = "BBBB2222"
            calcengine_result_2plus3 = "CCCC3333"
            calcengine_menu_open = "DDDD4444"
        }

        $existingBaseline = [ordered]@{
            capturedAt = "2026-03-20T00:00:00.0000000Z"
            cemu_version = "old"
            rom_hash = "OLDROM"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $existingBaseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath,
            "-Force"
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Recorder should overwrite when forced."
        Assert-Contains -Needle "Existing baseline detected." -Haystack $run.output_text -Message "Recorder should print the diff preamble."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected 6 -Actual ([int]$summary.diff.changed_count) -Message "Diff should report six changed hashes."

        $baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "AAAA1111" -Actual ([string]$baseline.screens[0].hash) -Message "Recorder should overwrite the old shell hash."
        Assert-Equal -Expected "PPPPMMMM" -Actual ([string]$baseline.screens[2].hash) -Message "Recorder should overwrite the old PRGM-menu hash."
        Assert-Equal -Expected "DDDD4444" -Actual ([string]$baseline.screens[5].hash) -Message "Recorder should overwrite the old menu hash."
    }

    Invoke-TestCase -Name "Recorder cancels without -Force when baseline already exists" -Body {
        # When run non-interactively, Read-Host receives no stdin; the empty response
        # does not match the /y(es)?/ pattern, so the recorder cancels.
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Hashes @{
            artifice_shell_loaded = "AAAA1111"
            artifice_quit_transition = "AAAATTTT"
            ti_os_program_menu_open = "PPPPMMMM"
            calcengine_launch_menu = "BBBB2222"
            calcengine_result_2plus3 = "CCCC3333"
            calcengine_menu_open = "DDDD4444"
        }

        $existingBaseline = [ordered]@{
            capturedAt = "2026-03-20T00:00:00.0000000Z"
            cemu_version = "old"
            rom_hash = "OLDROM"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $existingBaseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        # Run WITHOUT -Force so that the empty stdin causes the prompt to decline.
        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Recorder should exit with failure code when run without -Force on an existing baseline."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should record failure."
        # In a non-interactive subprocess, Read-Host may throw rather than return empty;
        # either way overwrite_confirmed must remain false.
        Assert-Equal -Expected $false -Actual ([bool]$summary.overwrite_confirmed) -Message "Overwrite should not be confirmed when -Force is absent."

        # The existing baseline must remain unchanged.
        $unchanged = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "11111111" -Actual ([string]$unchanged.screens[0].hash) -Message "Existing baseline must not be overwritten when no confirmation is given."
    }

    Invoke-TestCase -Name "Recorder fails when no arTIfiCE asset is available" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $missingArtificePath = Join-Path $tempDir "does-not-exist-arTIfiCE.8xp"
        $missingPreferredAssetPath = Join-Path $tempDir "missing-artifice-repo.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Hashes @{
            artifice_shell_loaded = "11111111"
            artifice_quit_transition = "AAAAAAA1"
            ti_os_program_menu_open = "BBBBBBB2"
            calcengine_launch_menu = "22222222"
            calcengine_result_2plus3 = "33333333"
            calcengine_menu_open = "44444444"
        }

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $missingArtificePath,
            "-PreferredAssetPath", $missingPreferredAssetPath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath,
            "-Force"
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Recorder should fail when no arTIfiCE asset is available."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary should record failure."
        Assert-Equal -Expected $false -Actual ([bool]$summary.overwrite_confirmed) -Message "Overwrite should not be confirmed if capture failed."
        Assert-True -Condition (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) -Message "Baseline file must not be created when capture failed."
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
