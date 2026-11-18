Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).ProviderPath
$script:RunnerPath = Join-Path $script:RepoRoot "tools\emulator\run-artifice-smoke.ps1"
if (-not (Test-Path -LiteralPath $script:RunnerPath -PathType Leaf)) {
    Write-Error "arTIfiCE smoke runner was not found at '$script:RunnerPath'."
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
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("artifice-smoke-test-{0}" -f [System.Guid]::NewGuid().ToString("N"))
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
        [Parameter(Mandatory = $true)][string]$ObservationPath
    )

    $content = @"
param(
    [string]`$RomPath,
    [string]`$AutotesterPath,
    [string]`$EmulatorBackend,
    [string]`$Mode,
    [string]`$DownloadsDir,
    [string]`$TemplatePath,
    [string]`$GeneratedPath,
    [string]`$BaselinedPath,
    [string]`$CrcCatalogJsonPath,
    [string]`$CrcCatalogMarkdownPath,
    [string]`$SummaryPath
)

`$template = Get-Content -LiteralPath `$TemplatePath -Raw | ConvertFrom-Json
`$observation = [ordered]@{
    mode = `$Mode
    emulator_backend = `$EmulatorBackend
    downloads_dir = `$DownloadsDir
    transfer_files = @(
        Get-ChildItem -LiteralPath `$DownloadsDir -File |
        Sort-Object Name |
        Select-Object -ExpandProperty Name
    )
    template_target = [string]`$template.target.name
    template_is_asm = [bool]`$template.target.isASM
    hashes = @(
        `$template.hashes.PSObject.Properties |
        Select-Object -ExpandProperty Name
    )
}
`$observation | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath '$ObservationPath' -Encoding UTF8

`$summary = [ordered]@{
    schema_version = 1
    success = `$true
    exit_code = 0
    mode = `$Mode
}
`$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath `$SummaryPath -Encoding UTF8
Write-Host 'fake artifice smoke runner ok'
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
    Invoke-TestCase -Name "Smoke runner fails fast when no arTIfiCE asset is available" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $summaryPath = Join-Path $tempDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $observationPath = Join-Path $tempDir "observation.json"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $missingArtificePath = Join-Path $tempDir "missing-artifice.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $observationPath

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $missingArtificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 1 -Actual ([int]$run.exit_code) -Message "Runner should fail when no arTIfiCE asset is available."
        Assert-Contains -Needle "Explicit arTIfiCE path" -Haystack $run.output_text -Message "Failure output should explain the missing explicit asset path."
        Assert-True -Condition (-not (Test-Path -LiteralPath $observationPath -PathType Leaf)) -Message "Delegated runner should not run when the asset is missing."
    }

    Invoke-TestCase -Name "Smoke runner stages A.8xp and delegates requested mode/backend" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $summaryPath = Join-Path $tempDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $observationPath = Join-Path $tempDir "observation.json"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"
        $artificePath = Join-Path $tempDir "arTIfiCE_v2.1.8xp"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        New-BinaryFile -Path $artificePath -SizeBytes 978
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $observationPath

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-EmulatorBackend", "Auto",
            "-DownloadsDir", $downloadsDir,
            "-ArtificeProgramPath", $artificePath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-Mode", "Verify",
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Runner should succeed when dependencies are present."
        Assert-True -Condition (Test-Path -LiteralPath $observationPath -PathType Leaf) -Message "Delegated runner should have been invoked."

        $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected "Verify" -Actual ([string]$observation.mode) -Message "Requested mode should be forwarded."
        Assert-Equal -Expected "Auto" -Actual ([string]$observation.emulator_backend) -Message "Smoke runner should default to the shared Auto backend so headless proof can be used when available."
        Assert-Equal -Expected "A" -Actual ([string]$observation.template_target) -Message "Smoke template should target prgmA."
        Assert-Equal -Expected $false -Actual ([bool]$observation.template_is_asm) -Message "prgmA should be treated as a TI-Basic program."
        Assert-True -Condition (@($observation.transfer_files) -contains "A.8xp") -Message "Staged transfer files should include A.8xp."
        Assert-True -Condition (@($observation.hashes) -contains "artifice_quit_transition") -Message "Smoke suite should capture the post-shell quit transition."
        Assert-True -Condition (@($observation.hashes) -contains "ti_os_program_menu_open") -Message "Smoke suite should capture the verified TI-OS PRGM menu checkpoint."
        Assert-True -Condition (@($observation.hashes) -contains "calcengine_result_6times7") -Message "Smoke suite should include the 6*7 checkpoint."
        Assert-True -Condition (@($observation.hashes) -contains "calcengine_menu_closed") -Message "Smoke suite should include the menu-close checkpoint."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected "Auto" -Actual ([string]$summary.options.emulator_backend) -Message "Summary should record the requested delegated backend."
        Assert-Equal -Expected 3 -Actual (@($summary.transfer_files).Count) -Message "Summary should preserve the staged transfer inventory without duplicate archive entries."
        Assert-True -Condition (@($summary.transfer_files | ForEach-Object { [System.IO.Path]::GetFileName([string]$_) }) -contains "A.8xp") -Message "Summary transfer inventory should include the staged arTIfiCE program."
        Assert-Equal -Expected 1 -Actual (@($summary.transfer_files | ForEach-Object { [System.IO.Path]::GetFileName([string]$_) } | Where-Object { $_ -eq "A.8xp" }).Count) -Message "Summary transfer inventory should list A.8xp only once."
    }

    Invoke-TestCase -Name "Smoke runner can bootstrap the missing preferred arTIfiCE asset" -Body {
        $tempDir = New-TestDirectory
        $downloadsDir = Join-Path $tempDir "downloads"
        $summaryPath = Join-Path $tempDir "summary.json"
        $manifestPath = Join-Path $tempDir "manifest.json"
        $fakeRunnerPath = Join-Path $tempDir "fake-comprehensive.ps1"
        $observationPath = Join-Path $tempDir "observation.json"
        $bootstrapScriptPath = Join-Path $tempDir "fake-bootstrap.ps1"
        $preferredAssetPath = Join-Path $tempDir "assets\artifice-v2.8xp"
        $romPath = Join-Path $tempDir "Rom.rom"
        $programName = "CALCENG.8xp"
        $appVarName = "libload.8xv"

        [void](New-Item -ItemType Directory -Path $downloadsDir -Force)
        New-BinaryFile -Path $romPath -SizeBytes 4194304
        New-BinaryFile -Path (Join-Path $downloadsDir $programName) -SizeBytes 8192
        New-BinaryFile -Path (Join-Path $downloadsDir $appVarName) -SizeBytes 512
        Set-Content -LiteralPath (Join-Path $downloadsDir "README.md") -Value "readme" -Encoding UTF8
        New-MinimalManifest -Path $manifestPath -DownloadsDir $downloadsDir -ProgramName $programName -AppVarName $appVarName
        New-FakeComprehensiveRunner -Path $fakeRunnerPath -ObservationPath $observationPath
        New-FakeBootstrapScript -Path $bootstrapScriptPath -AssetContent "bootstrapped artifice payload"

        $run = Invoke-Runner -Arguments @(
            "-RomPath", $romPath,
            "-AutotesterPath", "C:\Users\user\Tools\CEmu\v2.0\autotester.exe",
            "-DownloadsDir", $downloadsDir,
            "-PreferredAssetPath", $preferredAssetPath,
            "-BootstrapArtificeAsset",
            "-ArtificeBootstrapScriptPath", $bootstrapScriptPath,
            "-ManifestPath", $manifestPath,
            "-ComprehensiveRunnerPath", $fakeRunnerPath,
            "-Mode", "Verify",
            "-OutputPath", $summaryPath
        )

        Assert-Equal -Expected 0 -Actual ([int]$run.exit_code) -Message "Smoke runner should succeed when bootstrap stages the missing asset."
        Assert-True -Condition (Test-Path -LiteralPath $preferredAssetPath -PathType Leaf) -Message "Bootstrap should create the preferred repo asset path."

        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        Assert-Equal -Expected $true -Actual ([bool]$summary.success) -Message "Summary should report success."
        Assert-Equal -Expected $true -Actual ([bool]$summary.artifice.bootstrap.attempted) -Message "Summary should record that bootstrap was attempted."
        Assert-Equal -Expected $true -Actual ([bool]$summary.artifice.bootstrap.success) -Message "Bootstrap should be marked successful."
        Assert-Contains -Needle "fake bootstrap ok" -Haystack (@($summary.artifice.bootstrap.output_lines) -join "`n") -Message "Bootstrap output should be preserved."
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
