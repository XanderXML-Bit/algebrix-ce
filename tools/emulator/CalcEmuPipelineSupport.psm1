Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-CalcEmuPipelineRepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..")).ProviderPath
}

function Remove-CalcEmuFilesystemProviderPrefix {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$PathValue = ""
    )

    $normalized = [string]$PathValue
    foreach ($prefix in @('Microsoft.PowerShell.Core\FileSystem::', 'FileSystem::')) {
        if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $normalized.Substring($prefix.Length)
        }
    }

    return $normalized
}

function Convert-CalcEmuWslUncPathToLinuxInfo {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ($PathValue -notmatch '^[\\]{2}wsl(?:\.localhost)?[\\]([^\\]+)[\\](.*)$') {
        return $null
    }

    $distro = [string]$matches[1]
    $remainder = [string]$matches[2]
    $linuxPath = '/' + ($remainder -replace '[\\]+', '/')
    return [ordered]@{
        distro = $distro
        linux_path = $linuxPath
    }
}

function Convert-CalcEmuLinuxMountPathToWindowsPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LinuxPath
    )

    if ($LinuxPath -notmatch '^/mnt/([a-zA-Z])/(.*)$') {
        return ""
    }

    $driveLetter = [string]$matches[1].ToUpperInvariant()
    $remainder = [string]$matches[2] -replace '/', '\\'
    return "${driveLetter}:\\$remainder"
}

function Resolve-CalcEmuWslSymlinkTargetPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    $normalizedPath = Remove-CalcEmuFilesystemProviderPrefix -PathValue $PathValue
    $linuxInfo = Convert-CalcEmuWslUncPathToLinuxInfo -PathValue $normalizedPath
    if ($null -eq $linuxInfo) {
        return $normalizedPath
    }

    $item = Get-Item -LiteralPath $normalizedPath -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) {
        return $normalizedPath
    }

    if (-not (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        return $normalizedPath
    }

    $wslCommand = Get-Command -Name 'wsl.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $wslCommand -or [string]::IsNullOrWhiteSpace([string]$wslCommand.Source)) {
        return $normalizedPath
    }

    $resolvedLinuxPath = ""
    try {
        $captured = & ([string]$wslCommand.Source) -d ([string]$linuxInfo.distro) -- readlink -f -- ([string]$linuxInfo.linux_path) 2>$null
        if ($LASTEXITCODE -eq 0) {
            $resolvedLinuxPath = [string](@($captured) | Select-Object -First 1)
        }
    }
    catch {
        return $normalizedPath
    }

    if ([string]::IsNullOrWhiteSpace($resolvedLinuxPath)) {
        return $normalizedPath
    }

    $windowsPath = Convert-CalcEmuLinuxMountPathToWindowsPath -LinuxPath $resolvedLinuxPath.Trim()
    if (-not [string]::IsNullOrWhiteSpace($windowsPath)) {
        return [System.IO.Path]::GetFullPath($windowsPath)
    }

    return $normalizedPath
}

function Resolve-CalcEmuPipelinePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    $normalizedPathValue = Remove-CalcEmuFilesystemProviderPrefix -PathValue $PathValue
    $repoRoot = Remove-CalcEmuFilesystemProviderPrefix -PathValue (Resolve-CalcEmuPipelineRepoRoot)
    $candidatePath = if ([System.IO.Path]::IsPathRooted($normalizedPathValue)) {
        [System.IO.Path]::GetFullPath($normalizedPathValue)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $normalizedPathValue))
    }

    return Resolve-CalcEmuWslSymlinkTargetPath -PathValue $candidatePath
}

function Join-CalcEmuOptionalPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$BasePath = "",

        [Parameter(Mandatory = $true)]
        [string]$ChildPath
    )

    if ([string]::IsNullOrWhiteSpace($BasePath)) {
        return ""
    }

    return (Join-Path $BasePath $ChildPath)
}

function Get-CalcEmuCrc32Hex {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Data
    )

    $mask = [uint64]4294967295
    $polynomial = [uint64]3988292384
    $crc = [uint64]4294967295

    foreach ($byteValue in $Data) {
        $crc = (($crc -bxor [uint64]$byteValue) -band $mask)
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) {
                $crc = ((($crc -shr 1) -bxor $polynomial) -band $mask)
            }
            else {
                $crc = (($crc -shr 1) -band $mask)
            }
        }
    }

    $final = (($crc -bxor $mask) -band $mask)
    return ("{0:X8}" -f [uint32]$final)
}

function Resolve-CalcEmuRomPath {
    param(
        [Parameter(Mandatory = $false)]
        [string]$RomPath = "rom\Rom.rom"
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $canonicalRelativePath = "rom\Rom.rom"
    $canonicalFullPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $canonicalRelativePath))
    $explicitRomSupplied = $PSBoundParameters.ContainsKey("RomPath") -and -not [string]::IsNullOrWhiteSpace($RomPath)

    $effectivePath = if ($explicitRomSupplied) {
        Resolve-CalcEmuPipelinePath -PathValue $RomPath
    }
    else {
        $canonicalFullPath
    }

    if (-not (Test-Path -LiteralPath $effectivePath -PathType Leaf)) {
        if ($explicitRomSupplied) {
            throw "ROM path '$effectivePath' does not exist. Supply -RomPath with an existing TI-84 CE ROM file or place the default ROM at '$canonicalFullPath'."
        }

        throw "ROM path could not be resolved. Supply -RomPath with an existing TI-84 CE ROM file or place the default ROM at '$canonicalFullPath'."
    }

    $resolvedEffectivePath = (Resolve-Path -LiteralPath $effectivePath).ProviderPath
    $resolvedCanonicalPath = if (Test-Path -LiteralPath $canonicalFullPath -PathType Leaf) {
        (Resolve-Path -LiteralPath $canonicalFullPath).ProviderPath
    }
    else {
        $canonicalFullPath
    }

    return [ordered]@{
        path = $resolvedEffectivePath
        canonical_path = $resolvedCanonicalPath
        source = if ($explicitRomSupplied) {
            "explicit"
        }
        else {
            "repo_default"
        }
    }
}

function Resolve-CalcEmuAutotesterPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$AutotesterPath = ""
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $candidateRows = New-Object System.Collections.Generic.List[object]
    $seenPaths = @{}

    function Add-Candidate {
        param(
            [Parameter(Mandatory = $true)]
            [string]$PathValue,

            [Parameter(Mandatory = $true)]
            [string]$Source
        )

        if ([string]::IsNullOrWhiteSpace($PathValue)) {
            return
        }

        $normalizedPath = [System.IO.Path]::GetFullPath($PathValue)
        $dedupeKey = $normalizedPath.ToUpperInvariant()
        if ($seenPaths.ContainsKey($dedupeKey)) {
            return
        }

        $seenPaths[$dedupeKey] = $true
        $candidateRows.Add([ordered]@{
            path = $normalizedPath
            source = $Source
            exists = (Test-Path -LiteralPath $normalizedPath -PathType Leaf)
        })
    }

    if (-not [string]::IsNullOrWhiteSpace($AutotesterPath)) {
        Add-Candidate -PathValue (Resolve-CalcEmuPipelinePath -PathValue $AutotesterPath) -Source "explicit"
    }

    if (-not [string]::IsNullOrWhiteSpace($env:CEMU_AUTOTESTER_PATH)) {
        Add-Candidate -PathValue $env:CEMU_AUTOTESTER_PATH -Source "env:CEMU_AUTOTESTER_PATH"
    }

    $command = Get-Command -Name "autotester.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace([string]$command.Source)) {
        Add-Candidate -PathValue ([string]$command.Source) -Source "PATH"
    }

    foreach ($workspaceCandidate in @(
        (Join-Path $repoRoot "tools\CEmu\autotester.exe"),
        (Join-Path $repoRoot "tools\cemu\autotester.exe"),
        (Join-Path $repoRoot ".tools\CEmu\autotester.exe"),
        (Join-Path $repoRoot ".tools\cemu\autotester.exe"),
        (Join-Path $repoRoot "third_party\CEmu\autotester.exe")
    )) {
        Add-Candidate -PathValue $workspaceCandidate -Source "workspace"
    }

    foreach ($commonCandidate in @(
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\autotester.exe"),
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\v2.0\autotester.exe"),
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\latest\autotester.exe"),
        "C:\Tools\CEmu\autotester.exe",
        "C:\tools\cemu\autotester.exe",
        (Join-CalcEmuOptionalPath -BasePath ${env:ProgramFiles} -ChildPath "CEmu\autotester.exe"),
        (Join-CalcEmuOptionalPath -BasePath ${env:ProgramFiles(x86)} -ChildPath "CEmu\autotester.exe"),
        (Join-CalcEmuOptionalPath -BasePath $env:LOCALAPPDATA -ChildPath "Programs\CEmu\autotester.exe")
    )) {
        if (-not [string]::IsNullOrWhiteSpace($commonCandidate)) {
            Add-Candidate -PathValue $commonCandidate -Source "common_install"
        }
    }

    $userToolsRoot = Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu"
    if (-not [string]::IsNullOrWhiteSpace($userToolsRoot) -and (Test-Path -LiteralPath $userToolsRoot -PathType Container)) {
        foreach ($candidateFile in @(
            Get-ChildItem -LiteralPath $userToolsRoot -Filter "autotester.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object -Property FullName
        )) {
            Add-Candidate -PathValue $candidateFile.FullName -Source "common_install_scan"
        }
    }

    foreach ($row in $candidateRows.ToArray()) {
        if ($row.exists) {
            return [ordered]@{
                path = (Resolve-Path -LiteralPath $row.path).ProviderPath
                source = [string]$row.source
                checked_candidates = $candidateRows.ToArray()
            }
        }
    }

    $checkedList = @($candidateRows.ToArray() | ForEach-Object { "{0} ({1})" -f $_.path, $_.source })
    $checkedText = if ($checkedList.Count -gt 0) {
        $checkedList -join "; "
    }
    else {
        "<none>"
    }

    throw ("autotester.exe could not be resolved. Checked explicit path, " +
        "env:CEMU_AUTOTESTER_PATH, PATH, workspace tools directories, and common install locations. " +
        "Candidates: {0}" -f $checkedText)
}

function Resolve-CalcEmuGuiPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$CEmuPath = ""
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $candidateRows = New-Object System.Collections.Generic.List[object]
    $seenPaths = @{}

    function Add-Candidate {
        param(
            [Parameter(Mandatory = $true)]
            [string]$PathValue,

            [Parameter(Mandatory = $true)]
            [string]$Source
        )

        if ([string]::IsNullOrWhiteSpace($PathValue)) {
            return
        }

        $normalizedPath = [System.IO.Path]::GetFullPath($PathValue)
        $dedupeKey = $normalizedPath.ToUpperInvariant()
        if ($seenPaths.ContainsKey($dedupeKey)) {
            return
        }

        $seenPaths[$dedupeKey] = $true
        $candidateRows.Add([ordered]@{
            path = $normalizedPath
            source = $Source
            exists = (Test-Path -LiteralPath $normalizedPath -PathType Leaf)
        })
    }

    if (-not [string]::IsNullOrWhiteSpace($CEmuPath)) {
        Add-Candidate -PathValue (Resolve-CalcEmuPipelinePath -PathValue $CEmuPath) -Source "explicit"
    }

    foreach ($envCandidate in @(
        @{ Value = $env:CEMU_GUI_PATH; Source = "env:CEMU_GUI_PATH" },
        @{ Value = $env:CEMU_PATH; Source = "env:CEMU_PATH" }
    )) {
        if (-not [string]::IsNullOrWhiteSpace([string]$envCandidate.Value)) {
            Add-Candidate -PathValue ([string]$envCandidate.Value) -Source ([string]$envCandidate.Source)
        }
    }

    foreach ($commandName in @("CEmu.exe", "cemu.exe")) {
        $command = Get-Command -Name $commandName -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace([string]$command.Source)) {
            Add-Candidate -PathValue ([string]$command.Source) -Source "PATH"
        }
    }

    foreach ($workspaceCandidate in @(
        (Join-Path $repoRoot "tools\CEmu\CEmu.exe"),
        (Join-Path $repoRoot "tools\cemu\CEmu.exe"),
        (Join-Path $repoRoot ".tools\CEmu\CEmu.exe"),
        (Join-Path $repoRoot ".tools\cemu\CEmu.exe"),
        (Join-Path $repoRoot "third_party\CEmu\CEmu.exe")
    )) {
        Add-Candidate -PathValue $workspaceCandidate -Source "workspace"
    }

    foreach ($commonCandidate in @(
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\CEmu.exe"),
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\v2.0\CEmu.exe"),
        (Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu\latest\CEmu.exe"),
        "C:\Tools\CEmu\CEmu.exe",
        "C:\tools\cemu\CEmu.exe",
        (Join-CalcEmuOptionalPath -BasePath ${env:ProgramFiles} -ChildPath "CEmu\CEmu.exe"),
        (Join-CalcEmuOptionalPath -BasePath ${env:ProgramFiles(x86)} -ChildPath "CEmu\CEmu.exe"),
        (Join-CalcEmuOptionalPath -BasePath $env:LOCALAPPDATA -ChildPath "Programs\CEmu\CEmu.exe")
    )) {
        if (-not [string]::IsNullOrWhiteSpace($commonCandidate)) {
            Add-Candidate -PathValue $commonCandidate -Source "common_install"
        }
    }

    $userToolsRoot = Join-CalcEmuOptionalPath -BasePath $HOME -ChildPath "Tools\CEmu"
    if (-not [string]::IsNullOrWhiteSpace($userToolsRoot) -and (Test-Path -LiteralPath $userToolsRoot -PathType Container)) {
        foreach ($candidateFile in @(
            Get-ChildItem -LiteralPath $userToolsRoot -Filter "CEmu.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object -Property FullName
        )) {
            Add-Candidate -PathValue $candidateFile.FullName -Source "common_install_scan"
        }
    }

    foreach ($row in $candidateRows.ToArray()) {
        if ($row.exists) {
            return [ordered]@{
                path = (Resolve-Path -LiteralPath $row.path).ProviderPath
                source = [string]$row.source
                checked_candidates = $candidateRows.ToArray()
            }
        }
    }

    $checkedList = @($candidateRows.ToArray() | ForEach-Object { "{0} ({1})" -f $_.path, $_.source })
    $checkedText = if ($checkedList.Count -gt 0) {
        $checkedList -join "; "
    }
    else {
        "<none>"
    }

    throw ("CEmu.exe could not be resolved. Checked explicit path, " +
        "env:CEMU_GUI_PATH, env:CEMU_PATH, PATH, workspace tools directories, and common install locations. " +
        "Candidates: {0}" -f $checkedText)
}

function Resolve-CalcEmuCEmuHeadlessPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$CEmuHeadlessPath = ""
    )

    $repoRoot = Resolve-CalcEmuPipelineRepoRoot
    $candidateRows = New-Object System.Collections.Generic.List[object]
    $seenPaths = @{}

    function Add-Candidate {
        param(
            [Parameter(Mandatory = $true)]
            [string]$PathValue,

            [Parameter(Mandatory = $true)]
            [string]$Source
        )

        if ([string]::IsNullOrWhiteSpace($PathValue)) {
            return
        }

        $normalizedPath = [System.IO.Path]::GetFullPath($PathValue)
        $dedupeKey = $normalizedPath.ToUpperInvariant()
        if ($seenPaths.ContainsKey($dedupeKey)) {
            return
        }

        $seenPaths[$dedupeKey] = $true
        $candidateRows.Add([ordered]@{
            path = $normalizedPath
            source = $Source
            exists = (Test-Path -LiteralPath $normalizedPath -PathType Leaf)
        })
    }

    if (-not [string]::IsNullOrWhiteSpace($CEmuHeadlessPath)) {
        Add-Candidate -PathValue (Resolve-CalcEmuPipelinePath -PathValue $CEmuHeadlessPath) -Source "explicit"
    }

    foreach ($envCandidate in @(
        @{ Value = $env:CEMU_HEADLESS_PATH; Source = "env:CEMU_HEADLESS_PATH" },
        @{ Value = $env:CEMU_HEADLESS_EXE; Source = "env:CEMU_HEADLESS_EXE" }
    )) {
        if (-not [string]::IsNullOrWhiteSpace([string]$envCandidate.Value)) {
            Add-Candidate -PathValue ([string]$envCandidate.Value) -Source ([string]$envCandidate.Source)
        }
    }

    foreach ($commandName in @("cemu-headless.exe", "cemu-headless")) {
        $command = Get-Command -Name $commandName -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace([string]$command.Source)) {
            Add-Candidate -PathValue ([string]$command.Source) -Source "PATH"
        }
    }

    foreach ($workspaceCandidate in @(
        (Join-Path $repoRoot "tools\emulator\cemu-headless\build\Release\cemu-headless.exe"),
        (Join-Path $repoRoot "tools\emulator\cemu-headless\build\cemu-headless.exe"),
        (Join-Path $repoRoot "tools\emulator\cemu-headless\cmake-build-release\cemu-headless.exe"),
        (Join-Path $repoRoot "tools\emulator\cemu-headless\out\Release\cemu-headless.exe")
    )) {
        Add-Candidate -PathValue $workspaceCandidate -Source "workspace"
    }

    $searchRoot = Join-Path $repoRoot "tools\emulator\cemu-headless"
    if (Test-Path -LiteralPath $searchRoot -PathType Container) {
        foreach ($candidateFile in @(
            Get-ChildItem -LiteralPath $searchRoot -Filter "cemu-headless.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object -Property FullName
        )) {
            Add-Candidate -PathValue $candidateFile.FullName -Source "workspace_scan"
        }
    }

    foreach ($row in $candidateRows.ToArray()) {
        if ($row.exists) {
            return [ordered]@{
                path = (Resolve-Path -LiteralPath $row.path).ProviderPath
                source = [string]$row.source
                checked_candidates = $candidateRows.ToArray()
            }
        }
    }

    $checkedList = @($candidateRows.ToArray() | ForEach-Object { "{0} ({1})" -f $_.path, $_.source })
    $checkedText = if ($checkedList.Count -gt 0) {
        $checkedList -join "; "
    }
    else {
        "<none>"
    }

    throw ("cemu-headless could not be resolved. Checked explicit path, " +
        "env:CEMU_HEADLESS_PATH, env:CEMU_HEADLESS_EXE, PATH, and common workspace build locations. " +
        "Candidates: {0}" -f $checkedText)
}

function Get-CalcEmuTransferManifest {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ManifestPath = ""
    )

    $resolvedManifestPath = if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        Join-Path (Resolve-CalcEmuPipelineRepoRoot) "tests\emulator\manifest.json"
    }
    else {
        Resolve-CalcEmuPipelinePath -PathValue $ManifestPath
    }

    if (-not (Test-Path -LiteralPath $resolvedManifestPath -PathType Leaf)) {
        throw "Transfer manifest '$resolvedManifestPath' was not found."
    }

    $manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json
    if ($null -eq $manifest.modules -or @($manifest.modules).Count -eq 0) {
        throw "Transfer manifest '$resolvedManifestPath' does not contain any module entries."
    }

    return [ordered]@{
        path = $resolvedManifestPath
        manifest = $manifest
        version = [string]$manifest.version
        module_count = @($manifest.modules).Count
        runtime_dependency_count = @($manifest.runtime_dependencies).Count
    }
}

function Test-CalcEmuTransferManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DirectoryPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ManifestPath = "",

        [Parameter(Mandatory = $false)]
        [switch]$ValidateModuleCrcs,

        [Parameter(Mandatory = $false)]
        [switch]$ValidateRuntimeDependencyCrcs,

        [Parameter(Mandatory = $false)]
        [switch]$RequireReadme
    )

    $resolvedDirectoryPath = Resolve-CalcEmuPipelinePath -PathValue $DirectoryPath
    if (-not (Test-Path -LiteralPath $resolvedDirectoryPath -PathType Container)) {
        throw "Transfer directory '$resolvedDirectoryPath' does not exist."
    }

    $manifestInfo = Get-CalcEmuTransferManifest -ManifestPath $ManifestPath
    $manifest = $manifestInfo.manifest
    $topLevelFiles = @(Get-ChildItem -LiteralPath $resolvedDirectoryPath -File | Sort-Object Name)
    $fileMap = @{}
    foreach ($file in $topLevelFiles) {
        $fileMap[$file.Name.ToUpperInvariant()] = $file
    }

    $missingModules = New-Object System.Collections.Generic.List[string]
    $moduleCrcMismatches = New-Object System.Collections.Generic.List[object]
    foreach ($module in @($manifest.modules)) {
        $moduleName = [string]$module.name
        $lookupKey = $moduleName.ToUpperInvariant()
        if (-not $fileMap.ContainsKey($lookupKey)) {
            $missingModules.Add($moduleName)
            continue
        }

        if ($ValidateModuleCrcs) {
            $file = $fileMap[$lookupKey]
            $actualCrc = Get-CalcEmuCrc32Hex -Data ([System.IO.File]::ReadAllBytes($file.FullName))
            $expectedCrc = [string]$module.crc32
            if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($actualCrc, $expectedCrc)) {
                $moduleCrcMismatches.Add([ordered]@{
                    name = $moduleName
                    expected_crc32 = $expectedCrc
                    observed_crc32 = $actualCrc
                })
            }
        }
    }

    $missingRuntimeDependencies = New-Object System.Collections.Generic.List[string]
    $runtimeDependencyCrcMismatches = New-Object System.Collections.Generic.List[object]
    foreach ($dependency in @($manifest.runtime_dependencies)) {
        $dependencyName = [string]$dependency.name
        $lookupKey = $dependencyName.ToUpperInvariant()
        if (-not $fileMap.ContainsKey($lookupKey)) {
            $missingRuntimeDependencies.Add($dependencyName)
            continue
        }

        if ($ValidateRuntimeDependencyCrcs) {
            $file = $fileMap[$lookupKey]
            $actualCrc = Get-CalcEmuCrc32Hex -Data ([System.IO.File]::ReadAllBytes($file.FullName))
            $expectedCrc = [string]$dependency.crc32
            if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($actualCrc, $expectedCrc)) {
                $runtimeDependencyCrcMismatches.Add([ordered]@{
                    name = $dependencyName
                    expected_crc32 = $expectedCrc
                    observed_crc32 = $actualCrc
                })
            }
        }
    }

    $expectedModuleKeys = @($manifest.modules | ForEach-Object { ([string]$_.name).ToUpperInvariant() })
    $unexpectedModules = @(
        $topLevelFiles |
        Where-Object { $_.Extension -ieq ".8xp" -and $_.Name.ToUpperInvariant() -notin $expectedModuleKeys } |
        Select-Object -ExpandProperty Name
    )

    $expectedRuntimeKeys = @($manifest.runtime_dependencies | ForEach-Object { ([string]$_.name).ToUpperInvariant() })
    $unexpectedRuntimeDependencies = @(
        $topLevelFiles |
        Where-Object { $_.Extension -ieq ".8xv" -and $_.Name.ToUpperInvariant() -notin $expectedRuntimeKeys } |
        Select-Object -ExpandProperty Name
    )

    $readmeRequired = [string]$manifest.readme.name
    $readmePresent = $false
    $readmeCrcMismatch = $null
    if (-not [string]::IsNullOrWhiteSpace($readmeRequired) -and $fileMap.ContainsKey($readmeRequired.ToUpperInvariant())) {
        $readmePresent = $true
        if ($RequireReadme) {
            $readmeFile = $fileMap[$readmeRequired.ToUpperInvariant()]
            if ($manifest.readme.PSObject.Properties.Name -contains "crc32") {
                $actualReadmeCrc = Get-CalcEmuCrc32Hex -Data ([System.IO.File]::ReadAllBytes($readmeFile.FullName))
                $expectedReadmeCrc = [string]$manifest.readme.crc32
                if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($actualReadmeCrc, $expectedReadmeCrc)) {
                    $readmeCrcMismatch = [ordered]@{
                        name = $readmeRequired
                        expected_crc32 = $expectedReadmeCrc
                        observed_crc32 = $actualReadmeCrc
                    }
                }
            }
        }
    }

    $moduleNames = @(
        $topLevelFiles |
        Where-Object { $_.Extension -ieq ".8xp" } |
        Select-Object -ExpandProperty Name
    )
    $runtimeNames = @(
        $topLevelFiles |
        Where-Object { $_.Extension -ieq ".8xv" } |
        Select-Object -ExpandProperty Name
    )

    $success = ($missingModules.Count -eq 0) -and
               ($moduleCrcMismatches.Count -eq 0) -and
               ($missingRuntimeDependencies.Count -eq 0) -and
               ($runtimeDependencyCrcMismatches.Count -eq 0) -and
               ($unexpectedModules.Count -eq 0) -and
               ($unexpectedRuntimeDependencies.Count -eq 0) -and
               ((-not $RequireReadme) -or ($readmePresent -and $null -eq $readmeCrcMismatch))

    return [ordered]@{
        manifest_path = $manifestInfo.path
        manifest_version = $manifestInfo.version
        downloads_dir = $resolvedDirectoryPath
        expected_module_count = @($manifest.modules).Count
        expected_runtime_dependency_count = @($manifest.runtime_dependencies).Count
        module_names = $moduleNames
        runtime_dependency_names = $runtimeNames
        missing_modules = $missingModules.ToArray()
        module_crc_mismatches = $moduleCrcMismatches.ToArray()
        unexpected_modules = $unexpectedModules
        missing_runtime_dependencies = $missingRuntimeDependencies.ToArray()
        runtime_dependency_crc_mismatches = $runtimeDependencyCrcMismatches.ToArray()
        unexpected_runtime_dependencies = $unexpectedRuntimeDependencies
        readme_required = $readmeRequired
        readme_present = $readmePresent
        readme_crc_mismatch = $readmeCrcMismatch
        success = $success
    }
}

Export-ModuleMember -Function `
    Get-CalcEmuCrc32Hex, `
    Get-CalcEmuTransferManifest, `
    Resolve-CalcEmuAutotesterPath, `
    Resolve-CalcEmuGuiPath, `
    Resolve-CalcEmuCEmuHeadlessPath, `
    Resolve-CalcEmuPipelinePath, `
    Resolve-CalcEmuPipelineRepoRoot, `
    Resolve-CalcEmuRomPath, `
    Test-CalcEmuTransferManifest
