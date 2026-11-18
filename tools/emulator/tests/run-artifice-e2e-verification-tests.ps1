Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-artifice-e2e-verification.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "arTIfiCE E2E runner was not found at '$script:RunnerPath'."
    exit 2
}

$script:TestTotal = 0
$script:TestPassed = 0
$script:TestFailed = 0
$script:CreatedTempDirectories = New-Object System.Collections.Generic.List[string]
$script:CreatedCleanupPaths = New-Object System.Collections.Generic.List[string]

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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("artifice-e2e-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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

function Ensure-CanonicalRepoRomFixture {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRomPath
    )

    $romDirectory = Join-Path $script:RepoRoot "rom"
    $romPath = Join-Path $romDirectory "Rom.rom"
    if (-not (Test-Path -LiteralPath $romDirectory -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $romDirectory -Force)
        $script:CreatedCleanupPaths.Add($romDirectory)
    }

    Copy-Item -LiteralPath $SourceRomPath -Destination $romPath -Force
    $script:CreatedCleanupPaths.Add($romPath)
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
        [Parameter(Mandatory = $true)][hashtable]$Hashes,
        [Parameter(Mandatory = $false)][AllowEmptyString()][string]$ObservationPath = ""
    )

    $observationWrite = ""
    if (-not [string]::IsNullOrWhiteSpace($ObservationPath)) {
        $observationWrite = @"
`$template = Get-Content -LiteralPath `$TemplatePath -Raw | ConvertFrom-Json
`$observation = [ordered]@{
    rom_path = `$RomPath
    autotester_path = `$AutotesterPath
    mode = `$Mode
    downloads_dir = `$DownloadsDir
    transfer_files = @(
        Get-ChildItem -LiteralPath `$DownloadsDir -File |
        Sort-Object Name |
        Select-Object -ExpandProperty Name
    )
    target_name = [string]`$template.target.name
    target_is_asm = [bool]`$template.target.isASM
    sequence = [string[]]@(`$template.sequence)
}
`$observation | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath '$ObservationPath' -Encoding UTF8
"@
    }

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

$observationWrite

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
Write-Host 'fake artifice verification runner ok'
exit 0
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
}

function New-FakeBootstrapScript {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AssetContent
    )

    $escapedContent = $AssetContent.Replace("'", "''")
    $content = @"
param(
    [string]`$OutputPath = ''
)

if ([string]::IsNullOrWhiteSpace(`$OutputPath)) {
    throw 'OutputPath is required for the fake bootstrap script.'
}

`$parent = Split-Path -Path `$OutputPath -Parent
if (-not [string]::IsNullOrWhiteSpace(`$parent) -and -not (Test-Path -LiteralPath `$parent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path `$parent -Force)
}

Set-Content -LiteralPath `$OutputPath -Value '$escapedContent' -Encoding ASCII
Write-Host 'fake bootstrap ok'
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
    Invoke-TestCase -Name "Missing arTIfiCE asset reports blocker and skips delegated runner" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $observationPath = Join-Path $tempDir "delegated-observation.json"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $missingPreferredAssetPath = Join-Path $tempDir "missing-artifice-repo.8xp"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $observationPath -Hashes @{
            artifice_shell_loaded = "11111111"
            artifice_quit_transition = "AAAAAAA1"
            ti_os_program_menu_open = "BBBBBBB2"
            calcengine_launch_menu = "22222222"
            calcengine_result_2plus3 = "33333333"
            calcengine_menu_open = "44444444"
        }

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-PreferredAssetPath", $missingPreferredAssetPath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when no arTIfiCE asset is available."
        Assert-Contains -Needle "No local arTIfiCE v2 .8xp asset was found" -Haystack $run.output_text -Message "Runner output should explain the blocker."
        Assert-True -Condition (-not (Test-Path -LiteralPath $observationPath -PathType Leaf)) -Message "Delegated comprehensive runner should not have been invoked."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $false -Actual ([bool]$summary.success) -Message "Summary success should be false."
    }

    Invoke-TestCase -Name "Baseline missing reports capture mode instead of pass" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
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
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should not pass without a recorded baseline."
        Assert-Contains -Needle "BASELINE_MISSING" -Haystack $run.output_text -Message "Runner should print the BASELINE_MISSING status."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "BASELINE_MISSING" -Actual ([string]$summary.comparison.overall_status) -Message "Summary should mark the overall comparison as BASELINE_MISSING."
        Assert-Equal -Expected 6 -Actual ([int]$summary.comparison.baseline_missing_count) -Message "All six screens should report baseline missing."
    }

    Invoke-TestCase -Name "Bootstrap stages arTIfiCE asset when the preferred repo copy is missing" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $preferredAssetPath = Join-Path $tempDir "assets\artifice-v2.8xp"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $bootstrapScriptPath = Join-Path $tempDir "fake-bootstrap.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
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
        New-FakeBootstrapScript -Path $bootstrapScriptPath -AssetContent "bootstrapped artifice payload"

        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00.0000000Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-PreferredAssetPath", $preferredAssetPath,
            "-BootstrapArtificeAsset",
            "-ArtificeBootstrapScriptPath", $bootstrapScriptPath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when bootstrap stages the missing asset."
        Assert-True -Condition (Test-Path -LiteralPath $preferredAssetPath -PathType Leaf) -Message "Bootstrap should create the preferred repo asset path."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected $true -Actual ([bool]$summary.capture.artifice.bootstrap.attempted) -Message "Summary should record that bootstrap was attempted."
        Assert-Equal -Expected $true -Actual ([bool]$summary.capture.artifice.bootstrap.success) -Message "Bootstrap should be marked successful."
        Assert-Equal -Expected $preferredAssetPath -Actual ([string]$summary.capture.artifice.bootstrap.preferred_asset_path) -Message "Bootstrap summary should record the preferred asset path."
        Assert-Contains -Needle "fake bootstrap ok" -Haystack (@($summary.capture.artifice.bootstrap.output_lines) -join "`n") -Message "Bootstrap output should be preserved."
    }

    Invoke-TestCase -Name "Matching baseline reports PASS for all six screens" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $observationPath = Join-Path $tempDir "delegated-observation.json"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $observationPath -Hashes @{
            artifice_shell_loaded = "11111111"
            artifice_quit_transition = "AAAAAAA1"
            ti_os_program_menu_open = "BBBBBBB2"
            calcengine_launch_menu = "22222222"
            calcengine_result_2plus3 = "33333333"
            calcengine_menu_open = "44444444"
        }

        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00.0000000Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when the capture matches the baseline."
        Assert-Contains -Needle "PASS  artifice_shell_loaded  11111111" -Haystack $run.output_text -Message "Runner should print PASS lines."

        $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "A" -Actual ([string]$observation.target_name) -Message "Delegated template should target program A."
        Assert-Equal -Expected $false -Actual ([bool]$observation.target_is_asm) -Message "Delegated template should treat arTIfiCE as a TI-Basic program."
        Assert-True -Condition (@($observation.transfer_files) -contains "A.8xp") -Message "Staged transfer set should include A.8xp."
        Assert-Contains -Needle "key|clear" -Haystack (@($observation.sequence) -join "`n") -Message "Template should use the verified CLEAR -> MODE shell exit choreography."
        Assert-Contains -Needle "hash|calcengine_menu_open" -Haystack (@($observation.sequence) -join "`n") -Message "Template should hash the opened CalcEngine menu."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "PASS" -Actual ([string]$summary.comparison.overall_status) -Message "Summary should record PASS."
        Assert-Equal -Expected 6 -Actual ([int]$summary.comparison.pass_count) -Message "All six screens should pass."
        Assert-Equal -Expected 0 -Actual ([int]$summary.comparison.fail_count) -Message "No screens should fail."
    }

    Invoke-TestCase -Name "Mismatched baseline reports FAIL with expected and actual hashes" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
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

        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00.0000000Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "99999999" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when a hash mismatches."
        Assert-Contains -Needle "FAIL  calcengine_launch_menu  expected=99999999 actual=22222222" -Haystack $run.output_text -Message "Runner should print mismatch details."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "FAIL" -Actual ([string]$summary.comparison.overall_status) -Message "Summary should record FAIL."
        Assert-Equal -Expected 1 -Actual ([int]$summary.comparison.fail_count) -Message "Exactly one screen should fail."
        Assert-Equal -Expected "FAIL" -Actual ([string]$summary.comparison.results[3].status) -Message "Launch-menu comparison should fail."
    }

    Invoke-TestCase -Name "Partial baseline mismatch reports correct per-screen statuses" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $outputsDir = Join-Path $tempDir "outputs"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $summaryPath = Join-Path $outputsDir "summary.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        [void](New-Item -ItemType Directory -Path $outputsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 4096
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        # Fake runner produces: shell=AA, transition=AT, prgm=PM, launch=BB, result=CC, menu=DD
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -Hashes @{
            artifice_shell_loaded = "AAAAAAAA"
            artifice_quit_transition = "AAAATTTT"
            ti_os_program_menu_open = "PPPPMMMM"
            calcengine_launch_menu = "BBBBBBBB"
            calcengine_result_2plus3 = "CCCCCCCC"
            calcengine_menu_open = "DDDDDDDD"
        }
        # Baseline: shell/transition/prgm/result match, launch WRONG, menu WRONG
        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00.0000000Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "AAAAAAAA" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAATTTT" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "PPPPMMMM" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "ZZZZZZZZ" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "CCCCCCCC" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "YYYYYYYY" }
            )
        }
        $baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when two hashes mismatch."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "FAIL" -Actual ([string]$summary.comparison.overall_status) -Message "Overall status should be FAIL."
        Assert-Equal -Expected 4 -Actual ([int]$summary.comparison.pass_count) -Message "Four screens should pass (shell, transition, PRGM menu, and result)."
        Assert-Equal -Expected 2 -Actual ([int]$summary.comparison.fail_count) -Message "Two screens should fail (launch and menu)."
        Assert-Equal -Expected "PASS" -Actual ([string]$summary.comparison.results[0].status) -Message "Shell screen should pass."
        Assert-Equal -Expected "PASS" -Actual ([string]$summary.comparison.results[1].status) -Message "Quit-transition screen should pass."
        Assert-Equal -Expected "PASS" -Actual ([string]$summary.comparison.results[2].status) -Message "PRGM-menu screen should pass."
        Assert-Equal -Expected "FAIL" -Actual ([string]$summary.comparison.results[3].status) -Message "Launch-menu screen should fail."
        Assert-Equal -Expected "PASS" -Actual ([string]$summary.comparison.results[4].status) -Message "Result screen should pass."
        Assert-Equal -Expected "FAIL" -Actual ([string]$summary.comparison.results[5].status) -Message "Menu-open screen should fail."
    }

    Invoke-TestCase -Name "Output directory is created when it does not exist" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $missingOutputDir = Join-Path $tempDir "new-outputs"
        $summaryPath = Join-Path $missingOutputDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $baselinePath = Join-Path $tempDir "artifice-e2e-baseline.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $romPath = Join-Path $tempDir "Rom.rom"
        $autotesterPath = Join-Path $tempDir "autotester.exe"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path $autotesterPath -SizeBytes 1024
        Ensure-CanonicalRepoRomFixture -SourceRomPath $romPath
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
        $baseline = [ordered]@{
            capturedAt = "2026-03-21T00:00:00.0000000Z"
            cemu_version = "test"
            rom_hash = "ROMHASH"
            screens = @(
                [ordered]@{ step = "artifice_shell_loaded"; description = "shell"; hash = "11111111" },
                [ordered]@{ step = "artifice_quit_transition"; description = "transition"; hash = "AAAAAAA1" },
                [ordered]@{ step = "ti_os_program_menu_open"; description = "prgm"; hash = "BBBBBBB2" },
                [ordered]@{ step = "calcengine_launch_menu"; description = "launch"; hash = "22222222" },
                [ordered]@{ step = "calcengine_result_2plus3"; description = "result"; hash = "33333333" },
                [ordered]@{ step = "calcengine_menu_open"; description = "menu"; hash = "44444444" }
            )
        }
        $baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $baselinePath -Encoding UTF8

        Assert-True -Condition (-not (Test-Path -LiteralPath $missingOutputDir -PathType Container)) `
            -Message "Precondition: output directory must not exist before the run."

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", $autotesterPath,
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-BaselinePath", $baselinePath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when all hashes match."
        Assert-True -Condition (Test-Path -LiteralPath $summaryPath -PathType Leaf) -Message "Summary file should have been created inside the auto-created output directory."
    }
}
finally {
    foreach ($path in ($script:CreatedCleanupPaths | Select-Object -Unique | Sort-Object Length -Descending)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
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
