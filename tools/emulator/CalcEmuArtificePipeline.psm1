Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$pipelineSupportModulePath = Join-Path $PSScriptRoot "CalcEmuPipelineSupport.psm1"
if (-not (Test-Path -LiteralPath $pipelineSupportModulePath -PathType Leaf)) {
    throw "Pipeline support module not found at '$pipelineSupportModulePath'."
}
Import-Module -Name $pipelineSupportModulePath -Force

function Ensure-CalcEmuParentDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $parent = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
}

function Write-CalcEmuUtf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    Ensure-CalcEmuParentDirectory -Path $Path
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Resolve-CalcEmuOptionalPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PathValue = ""
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    return Resolve-CalcEmuPipelinePath -PathValue $PathValue
}

function Resolve-CalcEmuExistingFilePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $resolvedPath = Resolve-CalcEmuPipelinePath -PathValue $PathValue
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "$Label '$resolvedPath' does not exist."
    }

    return (Resolve-Path -LiteralPath $resolvedPath).ProviderPath
}

function Normalize-CalcEmuHashValue {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$Value = ""
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    $hash = [string]$Value
    if ($hash.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $hash = $hash.Substring(2)
    }

    return $hash.PadLeft(8, "0").ToUpperInvariant()
}


function Normalize-CalcEmuFilesystemPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PathValue = ""
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    try {
        return Resolve-CalcEmuPipelinePath -PathValue $PathValue
    }
    catch {
        $normalized = [string]$PathValue
        foreach ($prefix in @('Microsoft.PowerShell.Core\FileSystem::', 'FileSystem::')) {
            if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $normalized = $normalized.Substring($prefix.Length)
                break
            }
        }

        try {
            return [System.IO.Path]::GetFullPath($normalized)
        }
        catch {
            return $normalized
        }
    }
}

function Find-CalcEmuArtificeCandidates {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ExplicitPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PreferredAssetPath = "",

        [Parameter(Mandatory = $false)]
        [string[]]$SearchRoots = @()
    )

    $rows = New-Object System.Collections.Generic.List[object]
    $seen = @{}

    function Add-Candidate {
        param(
            [Parameter(Mandatory = $true)]
            [string]$CandidatePath,

            [Parameter(Mandatory = $true)]
            [string]$Source
        )

        if ([string]::IsNullOrWhiteSpace($CandidatePath)) {
            return
        }

        $normalizedPath = Normalize-CalcEmuFilesystemPath -PathValue $CandidatePath
        $dedupeKey = $normalizedPath.ToUpperInvariant()
        if ($seen.ContainsKey($dedupeKey)) {
            return
        }

        $seen[$dedupeKey] = $true
        $rows.Add([ordered]@{
            path = $normalizedPath
            source = $Source
            exists = (Test-Path -LiteralPath $normalizedPath -PathType Leaf)
        })
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        Add-Candidate -CandidatePath $ExplicitPath -Source "explicit"
    }

    if (-not [string]::IsNullOrWhiteSpace($PreferredAssetPath)) {
        Add-Candidate -CandidatePath $PreferredAssetPath -Source "repo_asset"
    }

    foreach ($root in @($SearchRoots | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)) {
        $resolvedRoot = Normalize-CalcEmuFilesystemPath -PathValue $root
        if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
            $rows.Add([ordered]@{
                path = $resolvedRoot
                source = "search_root"
                exists = $false
            })
            continue
        }

        foreach ($candidate in @(
            Get-ChildItem -LiteralPath $resolvedRoot -File -Filter "*.8xp" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "(?i)artifice" } |
            Sort-Object -Property FullName
        )) {
            Add-Candidate -CandidatePath $candidate.FullName -Source "auto_scan"
        }
    }

    return $rows.ToArray()
}

function Resolve-CalcEmuArtificeAsset {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ExplicitPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PreferredAssetPath = "",

        [Parameter(Mandatory = $false)]
        [string[]]$SearchRoots = @()
    )

    $candidateRows = @(Find-CalcEmuArtificeCandidates -ExplicitPath $ExplicitPath -PreferredAssetPath $PreferredAssetPath -SearchRoots $SearchRoots)
    $existingCandidates = @($candidateRows | Where-Object { $_.exists -and (Test-Path -LiteralPath ([string]$_.path) -PathType Leaf) })
    $warnings = New-Object System.Collections.Generic.List[string]
    $selected = $null

    if ($existingCandidates.Count -gt 1) {
        $warnings.Add(("Multiple arTIfiCE candidates were found; selecting '{0}'." -f [string]$existingCandidates[0].path))
    }

    if ($existingCandidates.Count -gt 0) {
        $selected = $existingCandidates[0]
    }

    return [ordered]@{
        path = if ($selected) { (Resolve-Path -LiteralPath ([string]$selected.path)).ProviderPath } else { "" }
        source = if ($selected) { [string]$selected.source } else { "" }
        assets_present = ($null -ne $selected)
        warnings = $warnings.ToArray()
        checked_candidates = $candidateRows
    }
}

function Get-CalcEmuAlphaLaunchSequence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProgramName
    )

    # TI-84 Plus CE ALPHA layer exactly as printed on the keycaps (row order:
    # math..div = A..M, log..2 = N..Z). Must match src/core/launcher/main.c.
    $alphaMap = @{
        "A" = "math";  "B" = "apps"; "C" = "prgm"; "D" = "-1";  "E" = "sin"; "F" = "cos"; "G" = "tan"; "H" = "^";   "I" = "^2";
        "J" = ",";     "K" = "(";    "L" = ")";    "M" = "/";   "N" = "log"; "O" = "7";   "P" = "8";   "Q" = "9";
        "R" = "*";     "S" = "ln";   "T" = "4";    "U" = "5";   "V" = "6";   "W" = "-";   "X" = "sto"; "Y" = "1";   "Z" = "2"
    }

    $sequence = New-Object System.Collections.Generic.List[string]
    foreach ($charValue in $ProgramName.ToUpperInvariant().ToCharArray()) {
        $letter = [string]$charValue
        if (-not $alphaMap.ContainsKey($letter)) {
            throw "Program name '$ProgramName' contains unsupported character '$letter' for AutoTester alpha-key launch."
        }

        $sequence.Add("key|alpha")
        $sequence.Add(("key|{0}" -f [string]$alphaMap[$letter]))
    }

    return $sequence.ToArray()
}

function Get-CalcEmuArtificeScreenDefinitions {
    $steps = New-Object System.Collections.Generic.List[object]
    $steps.Add([ordered]@{
        step = "artifice_shell_loaded"
        description = "arTIfiCE v2 shell screen immediately after launching prgmA"
    })
    $steps.Add([ordered]@{
        step = "artifice_quit_transition"
        description = "Screen state after the verified CLEAR -> MODE shell-exit choreography, before reopening PRGM"
    })
    $steps.Add([ordered]@{
        step = "ti_os_program_menu_open"
        description = "TI-OS program menu after the verified CLEAR -> MODE -> PRGM arTIfiCE exit sequence"
    })
    $steps.Add([ordered]@{
        step = "calcengine_launch_menu"
        description = "CalcEngine landing screen after exiting arTIfiCE and launching CALCENG"
    })
    $steps.Add([ordered]@{
        step = "calcengine_result_2plus3"
        description = "CalcEngine result screen after evaluating 2+3"
    })
    $steps.Add([ordered]@{
        step = "calcengine_menu_open"
        description = "CalcEngine menu opened from the post-result state"
    })
    return $steps.ToArray()
}

function New-CalcEmuArtificeTemplateObject {
    param(
        [Parameter(Mandatory = $false)]
        [hashtable]$ExpectedHashes = @{}
    )

    $sequence = New-Object System.Collections.Generic.List[string]
    $sequence.Add("action|launch")
    $sequence.Add("delay|4000")
    $sequence.Add("hash|artifice_shell_loaded")
    $sequence.Add("key|clear")
    $sequence.Add("delay|1200")
    $sequence.Add("key|mode")
    $sequence.Add("delay|1200")
    $sequence.Add("hash|artifice_quit_transition")
    $sequence.Add("key|prgm")
    $sequence.Add("delay|1200")
    $sequence.Add("hash|ti_os_program_menu_open")
    foreach ($step in @(Get-CalcEmuAlphaLaunchSequence -ProgramName "CALCENG")) {
        $sequence.Add($step)
    }
    $sequence.Add("key|enter")
    $sequence.Add("delay|4000")
    $sequence.Add("hash|calcengine_launch_menu")
    $sequence.Add("key|2")
    $sequence.Add("key|+")
    $sequence.Add("key|3")
    $sequence.Add("delay|200")
    $sequence.Add("key|enter")
    $sequence.Add("delay|2000")
    $sequence.Add("hash|calcengine_result_2plus3")
    $sequence.Add("key|mode")
    $sequence.Add("delay|1200")
    $sequence.Add("hash|calcengine_menu_open")

    $hashes = [ordered]@{}
    foreach ($screen in @(Get-CalcEmuArtificeScreenDefinitions)) {
        $expectedHash = "00000000"
        if ($ExpectedHashes.ContainsKey([string]$screen.step) -and -not [string]::IsNullOrWhiteSpace([string]$ExpectedHashes[[string]$screen.step])) {
            $expectedHash = Normalize-CalcEmuHashValue -Value ([string]$ExpectedHashes[[string]$screen.step])
        }

        $hashes[[string]$screen.step] = [ordered]@{
            description = [string]$screen.description
            start = "vram_start"
            size = "vram_16_size"
            expected_CRCs = @($expectedHash)
            timeout = 8000
        }
    }

    return [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__ARTIFICE_AND_CALCENGINE_TRANSFER_FILES_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "A"
            isASM = $false
        }
        delay_after_key = 250
        delay_after_step = 600
        sequence = $sequence.ToArray()
        hashes = $hashes
    }
}

function Get-CalcEmuTransferFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DirectoryPath
    )

    return @(
        Get-ChildItem -LiteralPath $DirectoryPath -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -imatch "^\.(8xp|8xv)$" } |
        Sort-Object { $_.Extension }, { $_.Name } |
        Select-Object -ExpandProperty FullName
    )
}

function Stage-CalcEmuArtificeDownloadsDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDownloadsDir,

        [Parameter(Mandatory = $true)]
        [string]$ArtificeSourcePath,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDir
    )

    [void](New-Item -ItemType Directory -Path $DestinationDir -Force)
    $copiedFiles = New-Object System.Collections.Generic.List[string]

    foreach ($file in @(
        Get-ChildItem -LiteralPath $SourceDownloadsDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -imatch "^\.(8xp|8xv)$" } |
        Sort-Object Name
    )) {
        $destinationPath = Join-Path $DestinationDir $file.Name
        Copy-Item -LiteralPath $file.FullName -Destination $destinationPath -Force
        $copiedFiles.Add($destinationPath)
    }

    $stagedArtificePath = Join-Path $DestinationDir "A.8xp"
    Copy-Item -LiteralPath $ArtificeSourcePath -Destination $stagedArtificePath -Force
    $copiedFiles.Add($stagedArtificePath)

    return [ordered]@{
        destination_dir = $DestinationDir
        staged_artifice_path = $stagedArtificePath
        copied_file_count = $copiedFiles.Count
        copied_files = $copiedFiles.ToArray()
        transfer_count = @(Get-CalcEmuTransferFiles -DirectoryPath $DestinationDir).Count
    }
}

function Invoke-CalcEmuPowerShellFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $false)]
        [string[]]$Arguments = @()
    )

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).ProviderPath
    $outputLines = New-Object System.Collections.Generic.List[string]
    $exitCode = 1
    $invocationError = ""
    $startTime = Get-Date

    try {
        $captured = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $resolvedScriptPath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        foreach ($entry in @($captured)) {
            if ($null -ne $entry) {
                $outputLines.Add([string]$entry)
            }
        }
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    return [ordered]@{
        success = ([int]$exitCode -eq 0)
        exit_code = [int]$exitCode
        duration_ms = [int][Math]::Round(((Get-Date) - $startTime).TotalMilliseconds)
        output_lines = $outputLines.ToArray()
        invocation_error = $invocationError
    }
}

function Get-CalcEmuFileSha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [string](Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-CalcEmuFileVersionString {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $item = Get-Item -LiteralPath $Path
    $productVersion = [string]$item.VersionInfo.ProductVersion
    if (-not [string]::IsNullOrWhiteSpace($productVersion)) {
        return $productVersion
    }

    $fileVersion = [string]$item.VersionInfo.FileVersion
    if (-not [string]::IsNullOrWhiteSpace($fileVersion)) {
        return $fileVersion
    }

    return "unknown"
}

function Resolve-CalcEmuArtificeBootstrapScriptPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BootstrapScriptPath = ""
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $resolvedPath = if ([string]::IsNullOrWhiteSpace($BootstrapScriptPath)) {
        Join-Path $repoRoot "scripts\get-artifice-asset.ps1"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $BootstrapScriptPath
    }

    return [ordered]@{
        path = $resolvedPath
        exists = (-not [string]::IsNullOrWhiteSpace($resolvedPath) -and (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
    }
}

function Invoke-CalcEmuArtificeBootstrap {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BootstrapScriptPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PreferredAssetPath = ""
    )

    $scriptResolution = Resolve-CalcEmuArtificeBootstrapScriptPath -BootstrapScriptPath $BootstrapScriptPath
    if (-not [bool]$scriptResolution.exists) {
        return [ordered]@{
            attempted = $true
            success = $false
            exit_code = 1
            script_path = [string]$scriptResolution.path
            command = ""
            output_lines = @()
            invocation_error = "arTIfiCE bootstrap script was not found."
            preferred_asset_path = $PreferredAssetPath
            asset_present_after = $false
            asset_path_after = ""
            asset_sha256 = ""
            asset_size_bytes = 0
        }
    }

    $arguments = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($PreferredAssetPath)) {
        $arguments.Add("-OutputPath")
        $arguments.Add($PreferredAssetPath)
    }

    $invocation = Invoke-CalcEmuPowerShellFile -ScriptPath ([string]$scriptResolution.path) -Arguments $arguments.ToArray()
    $assetPathAfter = ""
    $assetSha256 = ""
    $assetSizeBytes = 0
    if (-not [string]::IsNullOrWhiteSpace($PreferredAssetPath) -and (Test-Path -LiteralPath $PreferredAssetPath -PathType Leaf)) {
        $resolvedAssetPath = (Resolve-Path -LiteralPath $PreferredAssetPath).ProviderPath
        $assetPathAfter = $resolvedAssetPath
        $assetSha256 = Get-CalcEmuFileSha256Hex -Path $resolvedAssetPath
        $assetSizeBytes = [int](Get-Item -LiteralPath $resolvedAssetPath).Length
    }

    return [ordered]@{
        attempted = $true
        success = ([bool]$invocation.success -and -not [string]::IsNullOrWhiteSpace($assetPathAfter))
        exit_code = [int]$invocation.exit_code
        script_path = [string]$scriptResolution.path
        command = ""
        output_lines = @($invocation.output_lines)
        invocation_error = [string]$invocation.invocation_error
        preferred_asset_path = $PreferredAssetPath
        asset_present_after = (-not [string]::IsNullOrWhiteSpace($assetPathAfter))
        asset_path_after = $assetPathAfter
        asset_sha256 = $assetSha256
        asset_size_bytes = $assetSizeBytes
    }
}

function Convert-CalcEmuHashMapToArtificeScreens {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Hashes
    )

    $screens = New-Object System.Collections.Generic.List[object]
    foreach ($screen in @(Get-CalcEmuArtificeScreenDefinitions)) {
        $step = [string]$screen.step
        $screens.Add([ordered]@{
            step = $step
            description = [string]$screen.description
            hash = if ($Hashes.ContainsKey($step)) { Normalize-CalcEmuHashValue -Value ([string]$Hashes[$step]) } else { "" }
        })
    }
    return $screens.ToArray()
}

function Get-CalcEmuArtificeScreensFromBaselinedConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        return @()
    }

    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $screens = New-Object System.Collections.Generic.List[object]
    foreach ($screen in @(Get-CalcEmuArtificeScreenDefinitions)) {
        $step = [string]$screen.step
        $hash = ""
        if ($config.hashes.PSObject.Properties.Name -contains $step) {
            $hashDef = $config.hashes.$step
            if ($null -ne $hashDef.expected_CRCs -and @($hashDef.expected_CRCs).Count -gt 0) {
                $hash = Normalize-CalcEmuHashValue -Value ([string]$hashDef.expected_CRCs[0])
            }
        }

        $screens.Add([ordered]@{
            step = $step
            description = [string]$screen.description
            hash = $hash
        })
    }

    return $screens.ToArray()
}

function New-CalcEmuArtificeBaselineObject {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RomPath,

        [Parameter(Mandatory = $true)]
        [string]$AutotesterPath,

        [Parameter(Mandatory = $true)]
        [object[]]$CapturedScreens
    )

    $screens = New-Object System.Collections.Generic.List[object]
    foreach ($screen in @($CapturedScreens)) {
        $screens.Add([ordered]@{
            step = [string]$screen.step
            description = [string]$screen.description
            hash = Normalize-CalcEmuHashValue -Value ([string]$screen.hash)
        })
    }

    return [ordered]@{
        capturedAt = (Get-Date -Format "o")
        cemu_version = Get-CalcEmuFileVersionString -Path $AutotesterPath
        rom_hash = Get-CalcEmuFileSha256Hex -Path $RomPath
        screens = $screens.ToArray()
    }
}

function Get-CalcEmuArtificeBaselineDiff {
    param(
        [Parameter(Mandatory = $true)]
        [object]$ExistingBaseline,

        [Parameter(Mandatory = $true)]
        [object]$NewBaseline
    )

    $existingMap = @{}
    foreach ($screen in @($ExistingBaseline.screens)) {
        $existingMap[[string]$screen.step] = $screen
    }

    $newMap = @{}
    foreach ($screen in @($NewBaseline.screens)) {
        $newMap[[string]$screen.step] = $screen
    }

    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($screenDef in @(Get-CalcEmuArtificeScreenDefinitions)) {
        $step = [string]$screenDef.step
        $existingHash = if ($existingMap.ContainsKey($step)) { Normalize-CalcEmuHashValue -Value ([string]$existingMap[$step].hash) } else { "" }
        $newHash = if ($newMap.ContainsKey($step)) { Normalize-CalcEmuHashValue -Value ([string]$newMap[$step].hash) } else { "" }
        $status = if ([string]::IsNullOrWhiteSpace($existingHash) -and -not [string]::IsNullOrWhiteSpace($newHash)) {
            "ADDED"
        }
        elseif (-not [string]::IsNullOrWhiteSpace($existingHash) -and [string]::IsNullOrWhiteSpace($newHash)) {
            "REMOVED"
        }
        elseif ([System.StringComparer]::OrdinalIgnoreCase.Equals($existingHash, $newHash)) {
            "UNCHANGED"
        }
        else {
            "CHANGED"
        }

        $rows.Add([ordered]@{
            step = $step
            description = [string]$screenDef.description
            old_hash = $existingHash
            new_hash = $newHash
            status = $status
        })
    }

    return [ordered]@{
        cemu_version_changed = (-not [System.StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$ExistingBaseline.cemu_version,
            [string]$NewBaseline.cemu_version))
        rom_hash_changed = (-not [System.StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$ExistingBaseline.rom_hash,
            [string]$NewBaseline.rom_hash))
        changed_count = @($rows | Where-Object { $_.status -eq "CHANGED" }).Count
        added_count = @($rows | Where-Object { $_.status -eq "ADDED" }).Count
        removed_count = @($rows | Where-Object { $_.status -eq "REMOVED" }).Count
        unchanged_count = @($rows | Where-Object { $_.status -eq "UNCHANGED" }).Count
        rows = $rows.ToArray()
    }
}

function Compare-CalcEmuArtificeCaptureToBaseline {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$CapturedScreens,

        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Baseline = $null
    )

    if ($null -eq $Baseline) {
        $results = New-Object System.Collections.Generic.List[object]
        foreach ($screen in @($CapturedScreens)) {
            $results.Add([ordered]@{
                step = [string]$screen.step
                description = [string]$screen.description
                status = "BASELINE_MISSING"
                expected_hash = ""
                actual_hash = Normalize-CalcEmuHashValue -Value ([string]$screen.hash)
            })
        }

        return [ordered]@{
            overall_status = "BASELINE_MISSING"
            pass_count = 0
            fail_count = 0
            baseline_missing_count = $results.Count
            results = $results.ToArray()
        }
    }

    $baselineMap = @{}
    foreach ($screen in @($Baseline.screens)) {
        $baselineMap[[string]$screen.step] = $screen
    }

    $results = New-Object System.Collections.Generic.List[object]
    foreach ($screen in @($CapturedScreens)) {
        $step = [string]$screen.step
        $actualHash = Normalize-CalcEmuHashValue -Value ([string]$screen.hash)
        $expectedHash = ""
        if ($baselineMap.ContainsKey($step)) {
            $expectedHash = Normalize-CalcEmuHashValue -Value ([string]$baselineMap[$step].hash)
        }

        $status = if (-not [string]::IsNullOrWhiteSpace($expectedHash) -and
            [System.StringComparer]::OrdinalIgnoreCase.Equals($expectedHash, $actualHash)) {
            "PASS"
        }
        else {
            "FAIL"
        }

        $results.Add([ordered]@{
            step = $step
            description = [string]$screen.description
            status = $status
            expected_hash = $expectedHash
            actual_hash = $actualHash
        })
    }

    $passCount = @($results | Where-Object { $_.status -eq "PASS" }).Count
    $failCount = @($results | Where-Object { $_.status -eq "FAIL" }).Count

    return [ordered]@{
        overall_status = if ($failCount -eq 0 -and $results.Count -gt 0) { "PASS" } else { "FAIL" }
        pass_count = $passCount
        fail_count = $failCount
        baseline_missing_count = 0
        results = $results.ToArray()
    }
}

function Invoke-CalcEmuArtificeHashCapture {
    param(
        [Parameter(Mandatory = $false)]
        [string]$RomPath = "rom\Rom.rom",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$AutotesterPath = "",

        [Parameter(Mandatory = $false)]
        [string]$DownloadsDir = "C:\Users\user\Downloads\CalcEngine",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ArtificeProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PreferredAssetPath = "",

        [Parameter(Mandatory = $false)]
        [switch]$BootstrapArtificeAsset,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ArtificeBootstrapScriptPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ManifestPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ComprehensiveRunnerPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$GeneratedPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BaselinedPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$CrcCatalogJsonPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$CrcCatalogMarkdownPath = ""
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $resolvedComprehensiveRunnerPath = if ([string]::IsNullOrWhiteSpace($ComprehensiveRunnerPath)) {
        Join-Path $repoRoot "tools\emulator\run-comprehensive-autotest.ps1"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $ComprehensiveRunnerPath
    }
    $resolvedGeneratedPath = if ([string]::IsNullOrWhiteSpace($GeneratedPath)) {
        Join-Path $repoRoot "tests\emulator\autotest.artifice-e2e.generated.json"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $GeneratedPath
    }
    $resolvedBaselinedPath = if ([string]::IsNullOrWhiteSpace($BaselinedPath)) {
        Join-Path $repoRoot "tests\emulator\autotest.artifice-e2e.baselined.json"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $BaselinedPath
    }
    $resolvedCrcCatalogJsonPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogJsonPath)) {
        Join-Path $repoRoot "tests\emulator\autotest.artifice-e2e.crc-catalog.json"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $CrcCatalogJsonPath
    }
    $resolvedCrcCatalogMarkdownPath = if ([string]::IsNullOrWhiteSpace($CrcCatalogMarkdownPath)) {
        Join-Path $repoRoot "tests\emulator\autotest.artifice-e2e.crc-catalog.md"
    }
    else {
        Resolve-CalcEmuOptionalPath -PathValue $CrcCatalogMarkdownPath
    }

    $result = [ordered]@{
        success = $false
        exit_code = 1
        options = [ordered]@{
            rom_path = $RomPath
            autotester_path = $AutotesterPath
            downloads_dir = $DownloadsDir
            artifice_program_path = $ArtificeProgramPath
            bootstrap_artifice_asset = [bool]$BootstrapArtificeAsset
            artifice_bootstrap_script_path = $ArtificeBootstrapScriptPath
            manifest_path = $ManifestPath
            comprehensive_runner_path = $resolvedComprehensiveRunnerPath
            generated_path = $resolvedGeneratedPath
            baselined_path = $resolvedBaselinedPath
            crc_catalog_json_path = $resolvedCrcCatalogJsonPath
            crc_catalog_markdown_path = $resolvedCrcCatalogMarkdownPath
        }
        package_validation = $null
        artifice = $null
        staged_downloads = $null
        delegated_runner = $null
        captured_screens = @()
        cemu_version = ""
        rom_hash = ""
        blockers = @()
        limitations = @(
            "This capture path uses the official arTIfiCE v2 TI-Basic launcher flow and delegates screen hashing to the existing comprehensive CEmu runner.",
            "If BootstrapArtificeAsset is enabled and no local arTIfiCE payload is present, the runner will attempt to fetch the official upstream asset before failing."
        )
    }

    $stagingRoot = ""
    try {
        if (-not (Test-Path -LiteralPath $resolvedComprehensiveRunnerPath -PathType Leaf)) {
            throw "Comprehensive autotest runner was not found at '$resolvedComprehensiveRunnerPath'."
        }

        $resolvedRom = [string](Resolve-CalcEmuRomPath -RomPath $RomPath).path
        $resolvedAutotester = if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
            Resolve-CalcEmuExistingFilePath -PathValue $AutotesterPath -Label "Autotester path"
        }
        else {
            [string](Resolve-CalcEmuAutotesterPath -AutotesterPath $AutotesterPath).path
        }
        $resolvedDownloadsDir = Resolve-CalcEmuPipelinePath -PathValue $DownloadsDir

        if (-not (Test-Path -LiteralPath $resolvedDownloadsDir -PathType Container)) {
            throw "DownloadsDir '$resolvedDownloadsDir' does not exist."
        }

        $result.options.rom_path = $resolvedRom
        $result.options.autotester_path = $resolvedAutotester
        $result.options.downloads_dir = $resolvedDownloadsDir
        $result.cemu_version = Get-CalcEmuFileVersionString -Path $resolvedAutotester
        $result.rom_hash = Get-CalcEmuFileSha256Hex -Path $resolvedRom

        $manifestValidation = Test-CalcEmuTransferManifest `
            -DirectoryPath $resolvedDownloadsDir `
            -ManifestPath $ManifestPath `
            -ValidateModuleCrcs `
            -ValidateRuntimeDependencyCrcs `
            -RequireReadme
        $result.package_validation = $manifestValidation

        if (-not $manifestValidation.success) {
            $result.blockers = @(
                "CalcEngine transfer package validation failed against the manifest; arTIfiCE verification should not run on an invalid package."
            )
            throw "CalcEngine transfer package validation failed."
        }

        $resolvedExplicitArtificePath = Resolve-CalcEmuOptionalPath -PathValue $ArtificeProgramPath
        if (-not [string]::IsNullOrWhiteSpace($resolvedExplicitArtificePath) -and
            -not (Test-Path -LiteralPath $resolvedExplicitArtificePath -PathType Leaf)) {
            $result.blockers = @(
                "Explicit arTIfiCE path '$resolvedExplicitArtificePath' does not exist."
            )
            throw "Explicit arTIfiCE path was not found."
        }
        $preferredAssetPath = if ([string]::IsNullOrWhiteSpace($PreferredAssetPath)) {
            Join-Path $repoRoot "tools\emulator\assets\artifice-v2.8xp"
        }
        else {
            Resolve-CalcEmuOptionalPath -PathValue $PreferredAssetPath
        }
        $resolvedArtifice = Resolve-CalcEmuArtificeAsset `
            -ExplicitPath $resolvedExplicitArtificePath `
            -PreferredAssetPath $preferredAssetPath `
            -SearchRoots @(
                $resolvedDownloadsDir,
                (Split-Path -Path $resolvedDownloadsDir -Parent),
                (Get-Location).Path
            )

        $bootstrap = [ordered]@{
            attempted = $false
            success = $false
            exit_code = 0
            script_path = if ([string]::IsNullOrWhiteSpace($ArtificeBootstrapScriptPath)) {
                (Join-Path $repoRoot "scripts\get-artifice-asset.ps1")
            }
            else {
                $ArtificeBootstrapScriptPath
            }
            output_lines = @()
            invocation_error = ""
            asset_present_after = $false
            asset_path_after = ""
            asset_sha256 = ""
            asset_size_bytes = 0
        }
        if (-not $resolvedArtifice.assets_present -and $BootstrapArtificeAsset) {
            $bootstrap = Invoke-CalcEmuArtificeBootstrap `
                -BootstrapScriptPath $ArtificeBootstrapScriptPath `
                -PreferredAssetPath $preferredAssetPath

            $resolvedArtifice = Resolve-CalcEmuArtificeAsset `
                -ExplicitPath $resolvedExplicitArtificePath `
                -PreferredAssetPath $preferredAssetPath `
                -SearchRoots @(
                    $resolvedDownloadsDir,
                    (Split-Path -Path $resolvedDownloadsDir -Parent),
                    (Get-Location).Path
                )
        }

        $result.artifice = [ordered]@{
            mode = "v2_basic_program"
            source = [string]$resolvedArtifice.source
            assets_present = [bool]$resolvedArtifice.assets_present
            program_path = [string]$resolvedArtifice.path
            repo_asset_path = $preferredAssetPath
            sha256 = if ([bool]$resolvedArtifice.assets_present) { Get-CalcEmuFileSha256Hex -Path ([string]$resolvedArtifice.path) } else { "" }
            size_bytes = if ([bool]$resolvedArtifice.assets_present) { [int](Get-Item -LiteralPath ([string]$resolvedArtifice.path)).Length } else { 0 }
            launch_target_name = "A"
            launch_sequence = @(
                "Transfer arTIfiCE v2 .8xp",
                "Run prgmA",
                "Verify shell screen hash",
                "Send CLEAR then MODE to leave the shell",
                "Verify the post-shell quit transition screen",
                "Open the TI-OS PRGM menu",
                "Verify the TI-OS PRGM menu before typing CALCENG",
                "Alpha-type CALCENG from the PRGM menu",
                "Launch CalcEngine",
                "Verify 2+3 result",
                "Open CalcEngine menu"
            )
            warnings = @($resolvedArtifice.warnings)
            checked_candidates = @($resolvedArtifice.checked_candidates)
            bootstrap = $bootstrap
        }

        if (-not $resolvedArtifice.assets_present) {
            $result.blockers = if ([bool]$BootstrapArtificeAsset) {
                @(
                    "No local arTIfiCE v2 .8xp asset was found, and bootstrap did not stage one. Review the bootstrap output or run scripts/get-artifice-asset.ps1 manually."
                )
            }
            else {
                @(
                    "No local arTIfiCE v2 .8xp asset was found. Run scripts/get-artifice-asset.ps1, tools/emulator/check-artifice-asset.ps1, or place the upstream .8xp in tools/emulator/assets/artifice-v2.8xp."
                )
            }
            throw "arTIfiCE v2 asset was not found."
        }

        $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("calcengine-artifice-capture-{0}" -f [System.Guid]::NewGuid().ToString("N"))
        $stagedDownloadsDir = Join-Path $stagingRoot "downloads"
        $templatePath = Join-Path $stagingRoot "autotest.artifice-e2e.template.json"
        $runnerSummaryPath = Join-Path $stagingRoot "artifice-e2e.runner.summary.json"
        $runnerGeneratedPath = Join-Path $stagingRoot "autotest.artifice.generated.json"
        $runnerBaselinedPath = Join-Path $stagingRoot "autotest.artifice.baselined.json"
        $runnerCatalogJsonPath = Join-Path $stagingRoot "autotest.artifice.crc-catalog.json"
        $runnerCatalogMarkdownPath = Join-Path $stagingRoot "autotest.artifice.crc-catalog.md"

        $result.staged_downloads = Stage-CalcEmuArtificeDownloadsDirectory `
            -SourceDownloadsDir $resolvedDownloadsDir `
            -ArtificeSourcePath ([string]$resolvedArtifice.path) `
            -DestinationDir $stagedDownloadsDir

        $template = New-CalcEmuArtificeTemplateObject
        Write-CalcEmuUtf8NoBom -Path $templatePath -Content ($template | ConvertTo-Json -Depth 12)

        $delegatedRunner = Invoke-CalcEmuPowerShellFile -ScriptPath $resolvedComprehensiveRunnerPath -Arguments @(
            "-RomPath", $resolvedRom,
            "-AutotesterPath", $resolvedAutotester,
            "-EmulatorBackend", "Autotester",
            "-Mode", "Baseline",
            "-DownloadsDir", $stagedDownloadsDir,
            "-TemplatePath", $templatePath,
            "-GeneratedPath", $runnerGeneratedPath,
            "-BaselinedPath", $runnerBaselinedPath,
            "-CrcCatalogJsonPath", $runnerCatalogJsonPath,
            "-CrcCatalogMarkdownPath", $runnerCatalogMarkdownPath,
            "-SummaryPath", $runnerSummaryPath
        )

        if (Test-Path -LiteralPath $runnerSummaryPath -PathType Leaf) {
            $delegatedRunner.summary_json = Get-Content -LiteralPath $runnerSummaryPath -Raw | ConvertFrom-Json
        }
        $result.delegated_runner = $delegatedRunner

        $capturedScreens = @()
        if (Test-Path -LiteralPath $runnerBaselinedPath -PathType Leaf) {
            $capturedScreens = @(Get-CalcEmuArtificeScreensFromBaselinedConfig -ConfigPath $runnerBaselinedPath)
        }

        if (@($capturedScreens).Count -eq 0 -and
            $null -ne $delegatedRunner.summary_json -and
            $null -ne $delegatedRunner.summary_json.baseline_stage -and
            $null -ne $delegatedRunner.summary_json.baseline_stage.observed_crcs) {
            $hashes = @{}
            foreach ($property in @($delegatedRunner.summary_json.baseline_stage.observed_crcs.PSObject.Properties)) {
                $hashes[[string]$property.Name] = [string]$property.Value
            }
            $capturedScreens = @(Convert-CalcEmuHashMapToArtificeScreens -Hashes $hashes)
        }

        $result.captured_screens = @($capturedScreens)
        $allHashesPresent = (@($capturedScreens | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.hash) }).Count -eq 0)
        $result.success = ([bool]$delegatedRunner.success -and $allHashesPresent)
        $result.exit_code = if ($result.success) { 0 } else { 1 }

        if (-not $result.success -and @($result.blockers).Count -eq 0) {
            $result.blockers = @(
                "arTIfiCE hash capture did not produce a complete six-screen snapshot set."
            )
        }
    }
    catch {
        if (@($result.blockers).Count -eq 0) {
            $result.blockers = @($_.Exception.Message)
        }
        $result.success = $false
        $result.exit_code = 1
    }
    finally {
        if (-not [string]::IsNullOrWhiteSpace($stagingRoot) -and (Test-Path -LiteralPath $stagingRoot)) {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    return $result
}

Export-ModuleMember -Function `
    Compare-CalcEmuArtificeCaptureToBaseline, `
    Convert-CalcEmuHashMapToArtificeScreens, `
    Ensure-CalcEmuParentDirectory, `
    Find-CalcEmuArtificeCandidates, `
    Get-CalcEmuAlphaLaunchSequence, `
    Get-CalcEmuArtificeBaselineDiff, `
    Get-CalcEmuArtificeScreenDefinitions, `
    Get-CalcEmuArtificeScreensFromBaselinedConfig, `
    Get-CalcEmuFileSha256Hex, `
    Get-CalcEmuFileVersionString, `
    Get-CalcEmuTransferFiles, `
    Invoke-CalcEmuArtificeHashCapture, `
    Invoke-CalcEmuPowerShellFile, `
    New-CalcEmuArtificeBaselineObject, `
    New-CalcEmuArtificeTemplateObject, `
    Normalize-CalcEmuHashValue, `
    Resolve-CalcEmuArtificeAsset, `
    Resolve-CalcEmuArtificeBootstrapScriptPath, `
    Resolve-CalcEmuOptionalPath, `
    Stage-CalcEmuArtificeDownloadsDirectory, `
    Invoke-CalcEmuArtificeBootstrap, `
    Write-CalcEmuUtf8NoBom
