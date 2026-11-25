[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom\Rom.rom",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

    [Parameter(Mandatory = $false)]
    [string]$DownloadsDir = "deploy/hardware-ready",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$ComprehensiveVerifySummaryPath = "",

    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$OutputPath = "",

    [Parameter(Mandatory = $false)]
    [switch]$SkipCEmuVerify,

    [Parameter(Mandatory = $false)]
    [switch]$SkipEz80Build,

    [Parameter(Mandatory = $false)]
    [switch]$SkipEz80DispatchSweep
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$script:StartTime = Get-Date
$script:MaxCapturedOutputLines = 200

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Ensure-ParentDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    Ensure-ParentDirectory -Path $Path
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $enc)
}

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

function Resolve-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return (Resolve-Path -LiteralPath $PathValue).Path
    }

    return (Resolve-Path -LiteralPath (Join-Path (Resolve-RepoRoot) $PathValue)).Path
}

function ConvertTo-TrimmedOutputLines {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [AllowNull()]
        [object[]]$Captured = @()
    )

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($Captured)) {
        if ($null -eq $entry) {
            continue
        }
        $lines.Add([string]$entry)
    }

    if ($lines.Count -le $script:MaxCapturedOutputLines) {
        return $lines.ToArray()
    }

    $headCount = [int][Math]::Floor(($script:MaxCapturedOutputLines - 1) / 2)
    $tailCount = $script:MaxCapturedOutputLines - $headCount - 1
    $trimmed = New-Object System.Collections.Generic.List[string]

    for ($index = 0; $index -lt $headCount; $index++) {
        $trimmed.Add($lines[$index])
    }

    $trimmed.Add(("... output truncated: {0} lines ..." -f $lines.Count))

    for ($index = $lines.Count - $tailCount; $index -lt $lines.Count; $index++) {
        $trimmed.Add($lines[$index])
    }

    return $trimmed.ToArray()
}

function Invoke-PowerShellFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$WorkingDirectory = ""
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $captured = New-Object System.Collections.Generic.List[string]
    $exitCode = 1
    $invocationError = ""
    $startTime = Get-Date

    try {
        $argumentParts = New-Object System.Collections.Generic.List[string]
        foreach ($part in @("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", $resolvedScriptPath) + @($Arguments)) {
            $partText = [string]$part
            if ($partText -match '[\s"]') {
                $argumentParts.Add(('"{0}"' -f $partText.Replace('"', '\"')))
            }
            else {
                $argumentParts.Add($partText)
            }
        }

        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = "powershell.exe"
        $startInfo.Arguments = ($argumentParts.ToArray() -join " ")
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            $startInfo.WorkingDirectory = $WorkingDirectory
        }

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        [void]$process.Start()

        $stdoutText = $process.StandardOutput.ReadToEnd()
        $stderrText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        $exitCode = [int]$process.ExitCode
        $process.Dispose()

        foreach ($line in @($stdoutText -split "`r?`n")) {
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                $captured.Add($line)
            }
        }
        foreach ($line in @($stderrText -split "`r?`n")) {
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                $captured.Add($line)
            }
        }
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    return [ordered]@{
        exit_code = $exitCode
        success = ($exitCode -eq 0)
        duration_ms = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
        output_lines = ConvertTo-TrimmedOutputLines -Captured $captured.ToArray()
        invocation_error = $invocationError
    }
}

function Get-PackageAudit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DownloadsPath
    )

    $manifestInfo = Get-CalcEmuTransferManifest
    $manifest = $manifestInfo.manifest
    $files = @(Get-ChildItem -LiteralPath $DownloadsPath -File)
    $programFiles = @($files | Where-Object { $_.Extension -ieq ".8xp" } | Sort-Object -Property Name)
    $appVarFiles = @($files | Where-Object { $_.Extension -ieq ".8xv" } | Sort-Object -Property Name)
    $validation = Test-CalcEmuTransferManifest `
        -DirectoryPath $DownloadsPath `
        -ValidateModuleCrcs `
        -ValidateRuntimeDependencyCrcs `
        -RequireReadme

    $artificeFiles = @(
        Get-ChildItem -LiteralPath $DownloadsPath -File -Recurse |
        Where-Object {
            $_.Name -match '(?i)artifice|cabri' -or
            $_.Extension -ieq '.8ek'
        } |
        Sort-Object -Property FullName |
        Select-Object -ExpandProperty FullName
    )

    return [ordered]@{
        downloads_dir = $DownloadsPath
        manifest_path = $manifestInfo.path
        manifest_version = $manifestInfo.version
        total_top_level_files = $files.Count
        top_level_program_count = $programFiles.Count
        top_level_appvar_count = $appVarFiles.Count
        expected_program_count = @($manifest.modules).Count
        expected_appvar_count = @($manifest.runtime_dependencies).Count
        readme_required = [string]$manifest.readme.name
        readme_present = [bool]$validation.readme_present
        missing_programs = @($validation.missing_modules)
        missing_appvars = @($validation.missing_runtime_dependencies)
        unexpected_programs = @($validation.unexpected_modules)
        unexpected_appvars = @($validation.unexpected_runtime_dependencies)
        program_crc_mismatches = @($validation.module_crc_mismatches)
        appvar_crc_mismatches = @($validation.runtime_dependency_crc_mismatches)
        readme_crc_mismatch = $validation.readme_crc_mismatch
        program_names = @($programFiles | Select-Object -ExpandProperty Name)
        appvar_names = @($appVarFiles | Select-Object -ExpandProperty Name)
        artifice_asset_paths = $artificeFiles
        artifice_assets_present = ($artificeFiles.Count -gt 0)
        success = [bool]$validation.success
        notes = @(
            "The package audit is manifest-driven and strict for the 35 program files, the 4 runtime .8xv AppVars, and README.md.",
            "arTIfiCE transfer assets are reported separately because the current CalcEngine package README treats them as an external prerequisite rather than bundled payloads."
        )
    }
}

function Convert-VersionTripletToText {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes
    )

    if ($Bytes.Length -ne 3) {
        return ""
    }

    return ("{0}.{1}.{2}" -f [int]$Bytes[2], [int]$Bytes[1], [int]$Bytes[0])
}

function Get-RomOs58Evidence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ResolvedRomPath
    )

    $bytes = [System.IO.File]::ReadAllBytes($ResolvedRomPath)
    $fileSize = [int64]$bytes.Length

    if ($fileSize -lt 0x3B002B) {
        throw "ROM '$ResolvedRomPath' is too small to contain the OS 5.8 certificate fields."
    }

    $certBytes = [byte[]]@($bytes[0x3B0028], $bytes[0x3B0029], $bytes[0x3B002A])
    $certVersion = Convert-VersionTripletToText -Bytes $certBytes
    $headerPattern = [byte[]]@(0x00, 0x08, 0x05, 0x00, 0x08, 0x05)
    $headerSearchStart = 0x020000
    $headerSearchEnd = 0x020040
    $headerMatchOffset = -1

    for ($offset = $headerSearchStart; $offset -le ($headerSearchEnd - $headerPattern.Length); $offset++) {
        $matched = $true
        for ($index = 0; $index -lt $headerPattern.Length; $index++) {
            if ($bytes[$offset + $index] -ne $headerPattern[$index]) {
                $matched = $false
                break
            }
        }

        if ($matched) {
            $headerMatchOffset = $offset
            break
        }
    }

    return [ordered]@{
        rom_path = $ResolvedRomPath
        file_size_bytes = $fileSize
        cert_version_bytes_hex = (@($certBytes | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
        cert_version = $certVersion
        header_search_range = ("0x{0:X6}-0x{1:X6}" -f $headerSearchStart, $headerSearchEnd)
        header_pattern_hex = (@($headerPattern | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
        header_match_offset = if ($headerMatchOffset -ge 0) { ("0x{0:X6}" -f $headerMatchOffset) } else { "" }
        success = ($certVersion -eq "5.8.0" -and $headerMatchOffset -ge 0)
        notes = @(
            "Certificate bytes at 0x3B0028..0x3B002A encode build/minor/major as 00 08 05 -> OS 5.8.0.",
            "The OS header probe scans 0x020000..0x020040 for the exact 00 08 05 00 08 05 signature instead of assuming a single fixed offset."
        )
    }
}

function Start-Ez80AgentSession {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CliPath
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $CliPath
    $startInfo.Arguments = '--agent'
    $startInfo.WorkingDirectory = Split-Path -Path $CliPath -Parent
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start ez80 agent process '$CliPath'."
    }

    return $process
}

function Invoke-Ez80AgentCommand {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [object]$Command
    )

    $jsonLine = $Command | ConvertTo-Json -Compress -Depth 8
    $Process.StandardInput.WriteLine($jsonLine)
    $Process.StandardInput.Flush()

    while ($true) {
        $responseLine = $Process.StandardOutput.ReadLine()
        if ([string]::IsNullOrWhiteSpace($responseLine)) {
            $stderrTail = $Process.StandardError.ReadToEnd()
            throw ("No response for agent command '{0}'. stderr: {1}" -f $Command.cmd, $stderrTail)
        }

        $response = $responseLine | ConvertFrom-Json
        $statusValue = if ($response.PSObject.Properties.Name -contains "status") {
            [string]$response.status
        } else {
            ""
        }

        if ($statusValue -eq "ready") {
            continue
        }

        if ($statusValue -ne "ok") {
            $errorValue = if ($response.PSObject.Properties.Name -contains "error") {
                [string]$response.error
            } else {
                $responseLine
            }
            throw ("Agent command '{0}' failed. Response: {1}" -f $Command.cmd, $errorValue)
        }

        return $response
    }
}

function Stop-Ez80AgentSession {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process
    )

    try {
        if (-not $Process.HasExited) {
            $Process.StandardInput.WriteLine('{"cmd":"quit","id":999999}')
            $Process.StandardInput.Flush()
            [void]$Process.StandardOutput.ReadLine()
            if (-not $Process.WaitForExit(5000)) {
                $Process.Kill()
            }
        }
    }
    finally {
        $Process.Dispose()
    }
}

function Invoke-Ez80DispatchSweep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CliPath,

        [Parameter(Mandatory = $true)]
        [string]$ResolvedRomPath,

        [Parameter(Mandatory = $true)]
        [string]$DownloadsPath
    )

    $programFiles = @(Get-ChildItem -LiteralPath $DownloadsPath -File -Filter "*.8xp" | Sort-Object -Property Name)
    $results = New-Object System.Collections.Generic.List[object]
    foreach ($programFile in $programFiles) {
        $programName = [System.IO.Path]::GetFileNameWithoutExtension($programFile.Name).ToUpperInvariant()
        $process = Start-Ez80AgentSession -CliPath $CliPath
        $commandId = 1

        try {
            [void](Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "set_os_version"
                id = $commandId
                major = 5
                minor = 8
                patch = 0
            }))
            $commandId++

            [void](Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "set_rom_loaded"
                id = $commandId
                loaded = $true
            }))
            $commandId++

            foreach ($keyValue in @(0x41, 0x52, 0x54, 0x49)) {
                [void](Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                    cmd = "inject_key"
                    id = $commandId
                    key = $keyValue
                }))
                $commandId++
            }

            [void](Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "load_8xp"
                id = $commandId
                path = $programFile.FullName
                archived = $true
            }))
            $commandId++

            $programListBefore = Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "list_programs"
                id = $commandId
            })
            $commandId++

            if ([int]$programListBefore.program_count -ne 1) {
                throw "Program '$programName' did not load as a single archived program."
            }

            $launchResponse = Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "launch_program"
                id = $commandId
                name = $programName
            })
            $commandId++

            $programListDuring = Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "list_programs"
                id = $commandId
            })
            $commandId++

            $activeDuring = @($programListDuring.programs | Where-Object { $_.active })
            $programRowDuring = @($programListDuring.programs | Where-Object { $_.name -eq $programName })[0]
            if ($activeDuring.Count -ne 1 -or $activeDuring[0].name -ne $programName) {
                throw "Program '$programName' did not become the sole active program after launch."
            }
            if ($null -eq $programRowDuring -or -not $programRowDuring.loaded_in_ram) {
                throw "Program '$programName' was not marked loaded_in_ram after launch."
            }

            $finishResponse = Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "finish_program"
                id = $commandId
                name = $programName
            })
            $commandId++

            $programListAfter = Invoke-Ez80AgentCommand -Process $process -Command ([ordered]@{
                cmd = "list_programs"
                id = $commandId
            })
            $commandId++

            $activeAfter = @($programListAfter.programs | Where-Object { $_.active })
            $programRowAfter = @($programListAfter.programs | Where-Object { $_.name -eq $programName })[0]
            if ($activeAfter.Count -ne 0) {
                throw "Program '$programName' remained active after finish_program."
            }
            if ($null -eq $programRowAfter -or $programRowAfter.loaded_in_ram) {
                throw "Program '$programName' did not return to archive after finish_program."
            }

            $results.Add([ordered]@{
                name = $programName
                load_program_count = [int]$programListBefore.program_count
                launch_ram_addr = [string]$launchResponse.ram_addr
                finish_archive_addr = [string]$finishResponse.archive_addr
                loaded_in_ram_after_launch = [bool]$programRowDuring.loaded_in_ram
                loaded_in_ram_after_finish = [bool]$programRowAfter.loaded_in_ram
            })
        }
        finally {
            Stop-Ez80AgentSession -Process $process
        }
    }

    return [ordered]@{
        success = $true
        os_version = "5.8.0"
        artifice_entry = "key_sequence_ARTI"
        program_count = $programFiles.Count
        program_names = @($programFiles | ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_.Name).ToUpperInvariant() })
        round_trip_results = $results.ToArray()
        notes = @(
            "The eZ80 scaffold sweep runs one fresh OS 5.8 plus arTIfiCE-bypass session per .8xp so each program can be loaded archived, launched, and returned to archive without hitting the scaffold's synthetic flash-capacity ceiling.",
            "The separate CEmu verify stage is the proof that the full 39-file package coexists and boots against the real ROM in one run."
        )
    }
}

$repoRoot = Resolve-RepoRoot
$resolvedRomPath = Resolve-CalcEmuRomPath -RomPath $RomPath
$resolvedRomPath = [string]$resolvedRomPath.path
$resolvedDownloadsDir = if ([System.IO.Path]::IsPathRooted($DownloadsDir)) {
    (Resolve-Path -LiteralPath $DownloadsDir).Path
} else {
    (Resolve-Path -LiteralPath (Join-Path $repoRoot $DownloadsDir)).Path
}

$resolvedOutputPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\comprehensive-environment-verification.summary.json"
} elseif ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath
} else {
    Join-Path $repoRoot $OutputPath
}

$resolvedCemuSummaryPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveVerifySummaryPath)) {
    Join-Path $repoRoot "tests\emulator\outputs\comprehensive-environment-verification.cemu.summary.json"
} elseif ([System.IO.Path]::IsPathRooted($ComprehensiveVerifySummaryPath)) {
    $ComprehensiveVerifySummaryPath
} else {
    Join-Path $repoRoot $ComprehensiveVerifySummaryPath
}

$summary = [ordered]@{
    schema_version = 1
    runner = "run-comprehensive-environment-verification.ps1"
    generated_at = (Get-Date -Format "o")
    success = $false
    exit_code = 1
    options = [ordered]@{
        rom_path = $resolvedRomPath
        downloads_dir = $resolvedDownloadsDir
        autotester_path = $AutotesterPath
        skip_cemu_verify = [bool]$SkipCEmuVerify
        skip_ez80_build = [bool]$SkipEz80Build
        skip_ez80_dispatch_sweep = [bool]$SkipEz80DispatchSweep
    }
    package_audit = $null
    rom_os58 = $null
    cemu_verify = $null
    ez80_build = $null
    ez80_dispatch = $null
    limitations = @(
        "CEmu AutoTester can prove the full 39-file package loads and the launcher boots on the real ROM, but it does not expose a robust per-module launcher-path API for all 35 programs in one deterministic run.",
        "The eZ80 scaffold sweep covers per-program archive->RAM->archive round-trips under modeled OS 5.8 plus arTIfiCE bypass using the scaffold's ROM-ready flag rather than a full 4 MB flash image; that avoids archive-capacity distortion while still exercising the TI-OS dispatch model.",
        "The current Downloads package does not bundle arTIfiCE payload files; the verifier reports that separately and treats jailbreak activation as an external prerequisite documented by the package README."
    )
}

try {
    if (-not (Test-Path -LiteralPath $resolvedDownloadsDir -PathType Container)) {
        throw "Downloads directory '$resolvedDownloadsDir' does not exist."
    }

    $summary.package_audit = Get-PackageAudit -DownloadsPath $resolvedDownloadsDir
    if (-not $summary.package_audit.success) {
        throw "Transfer package audit failed."
    }

    $summary.rom_os58 = Get-RomOs58Evidence -ResolvedRomPath $resolvedRomPath
    if (-not $summary.rom_os58.success) {
        throw "ROM OS-version probe did not resolve to 5.8.0."
    }

    $buildScriptPath = Join-Path $repoRoot "tools\emulator\ez80\build_tests.ps1"
    $cliPath = Join-Path $repoRoot "tools\emulator\ez80\ez80_cli.exe"

    if (-not $SkipEz80Build) {
        $summary.ez80_build = Invoke-PowerShellFile -ScriptPath $buildScriptPath -WorkingDirectory (Split-Path -Path $buildScriptPath -Parent)
        if (-not $summary.ez80_build.success) {
            throw "eZ80 CLI/runtime build failed."
        }
    }
    else {
        $summary.ez80_build = [ordered]@{
            success = $true
            exit_code = 0
            duration_ms = 0
            output_lines = @("Skipped build by request.")
            invocation_error = ""
        }
    }

    if (-not $SkipCEmuVerify) {
        $autotesterResolution = Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath
        $AutotesterPath = [string]$autotesterResolution.path
        $summary.options.autotester_path = $AutotesterPath

        $cemuScriptPath = Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
        $summary.cemu_verify = Invoke-PowerShellFile -ScriptPath $cemuScriptPath -Arguments @(
            "-RomPath", $resolvedRomPath,
            "-AutotesterPath", $AutotesterPath,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Verify",
            "-DownloadsDir", $resolvedDownloadsDir,
            "-SummaryPath", $resolvedCemuSummaryPath
        )
        if (-not $summary.cemu_verify.success) {
            throw "Comprehensive CEmu verify stage failed."
        }

        if (Test-Path -LiteralPath $resolvedCemuSummaryPath -PathType Leaf) {
            $summary.cemu_verify.summary_json = Get-Content -LiteralPath $resolvedCemuSummaryPath -Raw | ConvertFrom-Json
        }
    }
    else {
        $summary.cemu_verify = [ordered]@{
            success = $true
            exit_code = 0
            duration_ms = 0
            output_lines = @("Skipped CEmu verify by request.")
            invocation_error = ""
        }
    }

    if (-not $SkipEz80DispatchSweep) {
        if (-not (Test-Path -LiteralPath $cliPath -PathType Leaf)) {
            throw "Expected eZ80 CLI '$cliPath' does not exist after build."
        }
        $summary.ez80_dispatch = Invoke-Ez80DispatchSweep -CliPath $cliPath -ResolvedRomPath $resolvedRomPath -DownloadsPath $resolvedDownloadsDir
        if (-not $summary.ez80_dispatch.success) {
            throw "eZ80 dispatch sweep failed."
        }
    }
    else {
        $summary.ez80_dispatch = [ordered]@{
            success = $true
            notes = @("Skipped eZ80 dispatch sweep by request.")
        }
    }

    $summary.success = $true
    $summary.exit_code = 0
}
catch {
    $summary.success = $false
    $summary.exit_code = 1
    $summary.error = $_.Exception.Message
}
finally {
    $summary.duration_seconds = [Math]::Round(((Get-Date) - $script:StartTime).TotalSeconds, 2)
    Write-Utf8NoBom -Path $resolvedOutputPath -Content ($summary | ConvertTo-Json -Depth 12)
}

if ($summary.success) {
    Write-Host ("PASS: comprehensive environment verification succeeded. Summary: {0}" -f $resolvedOutputPath)
    exit 0
}

Write-Error ("FAIL: comprehensive environment verification failed. Summary: {0}" -f $resolvedOutputPath)
exit 1
