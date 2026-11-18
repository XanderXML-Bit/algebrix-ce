[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$HookArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date

$pipelineSupportModulePath = Join-Path $PSScriptRoot "..\emulator\CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

$artifactModulePath = Join-Path $PSScriptRoot "..\emulator\CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    throw "Artifact module not found at '$artifactModulePath'."
}
Import-Module -Name $artifactModulePath -Force

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

function Resolve-CommandPath {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        $resolved = Get-Command -Name $candidate -ErrorAction SilentlyContinue
        if ($resolved) {
            return $resolved.Source
        }
    }

    throw "Unable to resolve any of the following commands: $($Candidates -join ', ')"
}

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @(),
        [Parameter(Mandatory = $false)][string]$WorkingDirectory = ""
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (@($Arguments) -join " ")
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $startInfo.WorkingDirectory = $WorkingDirectory
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.StandardInput.Close()

    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($stdout -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $lines.Add($line)
        }
    }
    foreach ($line in @($stderr -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            $lines.Add($line)
        }
    }

    return [ordered]@{
        success = ($process.ExitCode -eq 0)
        exit_code = [int]$process.ExitCode
        output_lines = $lines.ToArray()
    }
}

function Add-StageResult {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)][System.Collections.Generic.List[object]]$Stages,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Success,
        [Parameter(Mandatory = $true)][int]$ExitCode,
        [Parameter(Mandatory = $true)][string]$Summary
    )

    $Stages.Add([ordered]@{
        name = $Name
        success = $Success
        exit_code = $ExitCode
        summary = $Summary
    })
}

function Invoke-ValidationStage {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)][System.Collections.Generic.List[object]]$Stages,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @(),
        [Parameter(Mandatory = $false)][string]$WorkingDirectory = "",
        [Parameter(Mandatory = $false)][int]$PreviewLineCount = 30
    )

    $result = Invoke-Process -FilePath $FilePath -Arguments $Arguments -WorkingDirectory $WorkingDirectory
    Add-StageResult -Stages $Stages -Name $Name -Success $result.success -ExitCode $result.exit_code -Summary $Summary

    if ($result.success) {
        Write-Host ("[PASS] {0}" -f $Name)
        return
    }

    Write-Host ("[FAIL] {0} (exit {1})" -f $Name, $result.exit_code)
    foreach ($line in @($result.output_lines | Select-Object -First $PreviewLineCount)) {
        Write-Host ("  {0}" -f $line)
    }
    exit 1
}

function Invoke-DirectValidationStage {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)][System.Collections.Generic.List[object]]$Stages,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $false)][string[]]$Arguments = @(),
        [Parameter(Mandatory = $false)][string]$WorkingDirectory = "",
        [Parameter(Mandatory = $false)][hashtable]$Environment = @{}
    )

    $previousLocation = Get-Location
    $originalEnvironment = @{}
    foreach ($entry in $Environment.GetEnumerator()) {
        $originalEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key)
        [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value)
    }

    try {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Push-Location -Path $WorkingDirectory
        }

        & $FilePath @Arguments
        $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    }
    finally {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Pop-Location
        }

        foreach ($entry in $Environment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $originalEnvironment[$entry.Key])
        }
    }

    $success = ($exitCode -eq 0)
    Add-StageResult -Stages $Stages -Name $Name -Success $success -ExitCode $exitCode -Summary $Summary

    if ($success) {
        Write-Host ("[PASS] {0}" -f $Name)
        return
    }

    Write-Host ("[FAIL] {0} (exit {1})" -f $Name, $exitCode)
    exit 1
}

function New-TemporaryDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Prefix
    )

    $basePath = [System.IO.Path]::GetTempPath()
    $directoryPath = Join-Path $basePath ("{0}{1}" -f $Prefix, [System.Guid]::NewGuid().ToString("N"))
    [void](New-Item -ItemType Directory -Path $directoryPath -Force)
    return $directoryPath
}

function New-StagedManifestTransferPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $artifactSet = Get-CalcEmuTransferManifestArtifacts -RepoRoot $RepoRoot
    if (@($artifactSet.errors).Count -gt 0) {
        throw ("Manifest-backed transfer package could not be resolved: {0}" -f (@($artifactSet.errors) -join "; "))
    }

    $stagingRoot = New-TemporaryDirectory -Prefix "calcengine-pre-push-"
    $downloadsDir = Join-Path $stagingRoot "downloads"
    [void](New-Item -ItemType Directory -Path $downloadsDir -Force)

    foreach ($artifactPath in @($artifactSet.all_paths)) {
        Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $downloadsDir (Split-Path -Path $artifactPath -Leaf)) -Force
    }

    return [ordered]@{
        root_dir = $stagingRoot
        downloads_dir = $downloadsDir
        transfer_count = @($artifactSet.all_paths).Count
        manifest_path = [string]$artifactSet.manifest_path
    }
}

function Test-TruthyEnvironmentValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $rawValue = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($rawValue)) {
        return $false
    }

    return @("1", "true", "yes", "on") -contains $rawValue.Trim().ToLowerInvariant()
}
function Add-SkippedStage {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)][System.Collections.Generic.List[object]]$Stages,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    Add-StageResult -Stages $Stages -Name $Name -Success $true -ExitCode 0 -Summary ("skipped: {0}" -f $Reason)
    Write-Host ("[WARN] {0}" -f $Reason) -ForegroundColor Yellow
    Write-Host ("[SKIP] {0}" -f $Name)
}

$repoRoot = Resolve-RepoRoot
$npmPath = Resolve-CommandPath -Candidates @("npm.cmd", "npm")
$nodePath = Resolve-CommandPath -Candidates @("node.exe", "node")
$powershellPath = Resolve-CommandPath -Candidates @("powershell.exe", "powershell")
$desktopRunner = Join-Path $repoRoot "tests\test_runner.exe"
$vitestEntryPath = Join-Path $repoRoot "node_modules\vitest\vitest.mjs"
$comprehensiveRunner = Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
$romPath = Join-Path $repoRoot "rom\Rom.rom"
$stagedTransferPackage = $null
$enableCemuVerify = Test-TruthyEnvironmentValue -Name "CALCENGINE_PRE_PUSH_CEMU_VERIFY"
$resolvedRomPath = ""
$autotesterPath = ""

try {
    $resolvedRomPath = [string](Resolve-CalcEmuRomPath -RomPath $romPath).path
}
catch {
    $resolvedRomPath = ""
}

try {
    $autotesterPath = [string](Resolve-CalcEmuAutotesterPath -AutotesterPath $env:CEMU_AUTOTESTER_PATH).path
}
catch {
    $autotesterPath = ""
}

$stages = New-Object System.Collections.Generic.List[object]

Write-Host "CalcEngine pre-push validation"

Invoke-ValidationStage -Stages $stages `
    -Name "powershell_harness" `
    -Summary "npm run --silent test:powershell-harness" `
    -FilePath $npmPath `
    -Arguments @("run", "--silent", "test:powershell-harness") `
    -WorkingDirectory $repoRoot `
    -PreviewLineCount 40

if (Test-Path -LiteralPath $vitestEntryPath -PathType Leaf) {
    Invoke-DirectValidationStage -Stages $stages `
        -Name "vitest" `
        -Summary "node node_modules/vitest/vitest.mjs run" `
        -FilePath $nodePath `
        -Arguments @(
            $vitestEntryPath,
            "run"
        ) `
        -WorkingDirectory $repoRoot `
        -Environment @{ CI = "1" }
}
else {
    Invoke-ValidationStage -Stages $stages `
        -Name "vitest" `
        -Summary "npm run --silent test:vitest" `
        -FilePath $npmPath `
        -Arguments @("run", "--silent", "test:vitest") `
        -WorkingDirectory $repoRoot `
        -PreviewLineCount 40
}

if (Test-Path -LiteralPath $desktopRunner -PathType Leaf) {
    Invoke-ValidationStage -Stages $stages `
        -Name "desktop_native" `
        -Summary "tests/test_runner.exe" `
        -FilePath $desktopRunner `
        -WorkingDirectory (Split-Path -Path $desktopRunner -Parent) `
        -PreviewLineCount 20
}
else {
    Add-SkippedStage -Stages $stages `
        -Name "desktop_native" `
        -Reason "Direct desktop native validation was skipped because tests/test_runner.exe is absent in this worktree."
}

$cemuEligible = (Test-Path -LiteralPath $comprehensiveRunner -PathType Leaf) -and
                (-not [string]::IsNullOrWhiteSpace($resolvedRomPath)) -and
                (-not [string]::IsNullOrWhiteSpace($autotesterPath)) -and
                (Test-Path -LiteralPath $autotesterPath -PathType Leaf)

if ($cemuEligible -and $enableCemuVerify) {
    $stagedTransferPackage = New-StagedManifestTransferPackage -RepoRoot $repoRoot
    Invoke-ValidationStage -Stages $stages `
        -Name "cemu_verify" `
        -Summary ("run-comprehensive-autotest.ps1 -Mode Verify (staged {0} manifest-backed transfer files)" -f [int]$stagedTransferPackage.transfer_count) `
        -FilePath $powershellPath `
        -Arguments @(
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            ('"{0}"' -f $comprehensiveRunner),
            "-RomPath",
            ('"{0}"' -f $resolvedRomPath),
            "-AutotesterPath",
            ('"{0}"' -f $autotesterPath),
            "-Mode",
            "Verify",
            "-DownloadsDir",
            ('"{0}"' -f [string]$stagedTransferPackage.downloads_dir)
        ) `
        -WorkingDirectory $repoRoot `
        -PreviewLineCount 30

    if ($null -ne $stagedTransferPackage -and
        -not [string]::IsNullOrWhiteSpace([string]$stagedTransferPackage.root_dir) -and
        (Test-Path -LiteralPath ([string]$stagedTransferPackage.root_dir) -PathType Container)) {
        Remove-Item -LiteralPath ([string]$stagedTransferPackage.root_dir) -Recurse -Force -ErrorAction SilentlyContinue
    }
}
elseif ($cemuEligible) {
    Add-SkippedStage -Stages $stages `
        -Name "cemu_verify" `
        -Reason "Comprehensive CEmu verify is disabled in pre-push by default because it is a long-running gate; set CALCENGINE_PRE_PUSH_CEMU_VERIFY=1 to enable it explicitly."
}
else {
    Add-SkippedStage -Stages $stages `
        -Name "cemu_verify" `
        -Reason "Comprehensive CEmu verify was skipped because rom/Rom.rom or autotester.exe could not be resolved."
}

$elapsed = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
Write-Host ""
Write-Host "Pre-push summary"
foreach ($stage in $stages) {
    $label = if ($stage.summary -like "skipped:*") { "SKIP" } elseif ($stage.success) { "PASS" } else { "FAIL" }
    Write-Host ("  [{0}] {1} - {2}" -f $label, $stage.name, $stage.summary)
}
Write-Host ("  Duration: {0}s" -f $elapsed)
exit 0
