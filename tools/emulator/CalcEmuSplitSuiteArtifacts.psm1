Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-CalcEmuMakeVariableTokens {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MakefilePath,

        [Parameter(Mandatory = $true)]
        [string]$VariableName
    )

    if (-not (Test-Path -LiteralPath $MakefilePath -PathType Leaf)) {
        throw "Makefile '$MakefilePath' was not found."
    }

    $lines = Get-Content -LiteralPath $MakefilePath
    $tokens = New-Object System.Collections.Generic.List[string]
    $collecting = $false

    foreach ($rawLine in $lines) {
        $lineText = [string]$rawLine
        $segment = ""

        if (-not $collecting) {
            $match = [System.Text.RegularExpressions.Regex]::Match(
                $lineText,
                ('^\s*{0}\s*:?\=\s*(.*)$' -f [System.Text.RegularExpressions.Regex]::Escape($VariableName))
            )
            if (-not $match.Success) {
                continue
            }

            $segment = [string]$match.Groups[1].Value
        }
        else {
            $segment = $lineText
        }

        $trimmedEnd = $segment.TrimEnd()
        $hasContinuation = $trimmedEnd.EndsWith("\", [System.StringComparison]::Ordinal)
        if ($hasContinuation) {
            $trimmedEnd = $trimmedEnd.Substring(0, $trimmedEnd.Length - 1)
        }

        foreach ($token in ($trimmedEnd -split '\s+')) {
            $candidate = ([string]$token).Trim()
            if ([string]::IsNullOrWhiteSpace($candidate)) {
                continue
            }

            $tokens.Add($candidate)
        }

        if (-not $hasContinuation) {
            $collecting = $false
            if ($tokens.Count -gt 0) {
                break
            }
        }
        else {
            $collecting = $true
        }
    }

    if ($tokens.Count -eq 0) {
        throw "Variable '$VariableName' was not found in '$MakefilePath'."
    }

    return $tokens.ToArray()
}

function Get-CalcEmuLargestArtifactPath {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [AllowNull()]
        [string[]]$CandidatePaths = @()
    )

    $largestPath = ""
    $largestSize = -1L

    foreach ($candidatePath in @($CandidatePaths)) {
        if ([string]::IsNullOrWhiteSpace([string]$candidatePath)) {
            continue
        }

        if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
            continue
        }

        $candidateSize = [int64](Get-Item -LiteralPath $candidatePath).Length
        if ($candidateSize -gt $largestSize) {
            $largestPath = (Resolve-Path -LiteralPath $candidatePath).ProviderPath
            $largestSize = $candidateSize
        }
    }

    return $largestPath
}

function Resolve-CalcEmuOfficialSplitSuiteArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    $rows = New-Object System.Collections.Generic.List[object]
    $modulePaths = New-Object System.Collections.Generic.List[string]
    $allPaths = New-Object System.Collections.Generic.List[string]

    $makefilePath = Join-Path $RepoRoot "Makefile"
    $programPath = ""

    try {
        $splitModules = @(Get-CalcEmuMakeVariableTokens -MakefilePath $makefilePath -VariableName "SPLIT_MODULES")
        $workerModules = @(Get-CalcEmuMakeVariableTokens -MakefilePath $makefilePath -VariableName "WORKER_MODULES")
        $officialModules = @($splitModules + $workerModules)

        foreach ($moduleName in $officialModules) {
            $directoryName = ([string]$moduleName).Replace(".", "_")
            $artifactDirectoryPath = Join-Path $RepoRoot ("bin\{0}" -f $directoryName)

            if (-not (Test-Path -LiteralPath $artifactDirectoryPath -PathType Container)) {
                $warnings.Add("Official split-suite directory '$artifactDirectoryPath' does not exist.")
                continue
            }

            $artifactFiles = @(Get-ChildItem -LiteralPath $artifactDirectoryPath -File -Filter "*.8xp" | Sort-Object -Property Name)
            if ($artifactFiles.Count -eq 0) {
                $warnings.Add("Official split-suite directory '$artifactDirectoryPath' does not contain a .8xp artifact.")
                continue
            }

            if ($artifactFiles.Count -gt 1) {
                $warnings.Add(
                    ("Official split-suite directory '{0}' contains {1} .8xp artifacts; using '{2}'." -f
                        $artifactDirectoryPath, $artifactFiles.Count, $artifactFiles[0].Name)
                )
            }

            $artifactFile = $artifactFiles[0]
            $resolvedArtifactPath = (Resolve-Path -LiteralPath $artifactFile.FullName).ProviderPath
            $artifactSizeBytes = [int64]$artifactFile.Length
            $isLauncher = ($directoryName -eq "launcher")

            $rows.Add([ordered]@{
                module_name = [string]$moduleName
                directory_name = $directoryName
                artifact_name = [string]$artifactFile.Name
                artifact_path = $resolvedArtifactPath
                size_bytes = $artifactSizeBytes
                launcher = $isLauncher
            })

            $allPaths.Add($resolvedArtifactPath)
            if ($isLauncher) {
                $programPath = $resolvedArtifactPath
            }
            else {
                $modulePaths.Add($resolvedArtifactPath)
            }
        }
    }
    catch {
        $errors.Add($_.Exception.Message)
    }

    if ([string]::IsNullOrWhiteSpace($programPath) -and $errors.Count -eq 0) {
        $warnings.Add("Official split-suite launcher artifact was not resolved from bin\\launcher.")
    }

    return [ordered]@{
        source = "official_split_suite"
        makefile_path = $makefilePath
        program_path = $programPath
        module_paths = $modulePaths.ToArray()
        all_paths = $allPaths.ToArray()
        artifacts = $rows.ToArray()
        errors = $errors.ToArray()
        warnings = $warnings.ToArray()
    }
}

function Get-CalcEmuTransferManifestArtifacts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ManifestPath = ""
    )

    if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        $ManifestPath = Join-Path $RepoRoot "tests\emulator\manifest.json"
    }

    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Transfer manifest '$ManifestPath' was not found."
    }

    $resolvedManifestPath = (Resolve-Path -LiteralPath $ManifestPath).ProviderPath
    $manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json
    if ($null -eq $manifest.modules -or @($manifest.modules).Count -eq 0) {
        throw "Transfer manifest '$resolvedManifestPath' does not contain any module entries."
    }

    $stagedRoot = Join-Path $RepoRoot "deploy\hardware-ready"
    $binRoot = Join-Path $RepoRoot "bin"
    $candidateRoots = @(
        [ordered]@{ path = $stagedRoot; source_priority = 0 },
        [ordered]@{ path = $binRoot; source_priority = 1 }
    )
    $availableArtifacts = @(
        foreach ($candidateRoot in $candidateRoots) {
            $directoryPath = [string]$candidateRoot.path
            if (-not (Test-Path -LiteralPath $directoryPath -PathType Container)) {
                continue
            }

            Get-ChildItem -LiteralPath $directoryPath -Recurse -File -Filter "*.8xp" |
            ForEach-Object {
                [ordered]@{
                    artifact_name = [string]$_.Name
                    artifact_path = (Resolve-Path -LiteralPath $_.FullName).ProviderPath
                    size_bytes = [int64]$_.Length
                    source_priority = [int]$candidateRoot.source_priority
                }
            }
        }
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    $rows = New-Object System.Collections.Generic.List[object]
    $modulePaths = New-Object System.Collections.Generic.List[string]
    $allPaths = New-Object System.Collections.Generic.List[string]
    $programPath = ""

    foreach ($manifestModule in @($manifest.modules)) {
        $manifestName = [string]$manifestModule.name
        $expectedSizeBytes = [int64]$manifestModule.size_bytes
        $exactMatches = @(
            $availableArtifacts |
            Where-Object { $_.artifact_name -ceq $manifestName -and [int64]$_.size_bytes -eq $expectedSizeBytes }
        )

        $selectedArtifact = $null
        if ($exactMatches.Count -eq 1) {
            $selectedArtifact = $exactMatches[0]
        }
        elseif ($exactMatches.Count -gt 1) {
            $sortedMatches = @(
                $exactMatches |
                Sort-Object -Property `
                    @{ Expression = { [int]$_.source_priority }; Descending = $false }, `
                    @{ Expression = { [string]$_.artifact_path }; Descending = $false }
            )
            $selectedArtifact = $sortedMatches[0]
            # A staged copy outranking an equal-size bin/ copy is the intended
            # priority (the normal state right after a package refresh), so only
            # warn when the top candidates tie on source priority.
            if ([int]$sortedMatches[1].source_priority -eq [int]$selectedArtifact.source_priority) {
                $warnings.Add(
                    ("Multiple exact matches found for '{0}'; using '{1}'." -f $manifestName, [string]$selectedArtifact.artifact_path)
                )
            }
        }
        else {
            $nameMatches = @(
                $availableArtifacts |
                Where-Object { $_.artifact_name -ceq $manifestName } |
                Sort-Object -Property `
                    @{ Expression = { [int]$_.source_priority }; Descending = $false }, `
                    @{ Expression = { [string]$_.artifact_path }; Descending = $false }
            )

            if ($nameMatches.Count -eq 1) {
                $selectedArtifact = $nameMatches[0]
                $warnings.Add(
                    ("Resolved '{0}' by filename only; expected {1} bytes but found {2} bytes." -f
                        $manifestName, $expectedSizeBytes, [int64]$selectedArtifact.size_bytes)
                )
            }
            else {
                $errors.Add(
                    ("Could not resolve manifest module '{0}' ({1} bytes) under '{2}' or '{3}'." -f $manifestName, $expectedSizeBytes, $stagedRoot, $binRoot)
                )
                continue
            }
        }

        $resolvedArtifactPath = [string]$selectedArtifact.artifact_path
        $rows.Add([ordered]@{
            artifact_name = $manifestName
            artifact_path = $resolvedArtifactPath
            size_bytes = [int64]$selectedArtifact.size_bytes
            expected_size_bytes = $expectedSizeBytes
            launcher = ($manifestName -ceq "CALCENG.8xp")
        })

        $allPaths.Add($resolvedArtifactPath)
        if ($manifestName -ceq "CALCENG.8xp") {
            $programPath = $resolvedArtifactPath
        }
        else {
            $modulePaths.Add($resolvedArtifactPath)
        }
    }

    if ([string]::IsNullOrWhiteSpace($programPath) -and $errors.Count -eq 0) {
        $warnings.Add("Manifest-backed transfer suite did not resolve CALCENG.8xp from deploy\\hardware-ready or bin.")
    }

    return [ordered]@{
        source = "manifest_transfer_suite"
        manifest_path = $resolvedManifestPath
        manifest_version = [string]$manifest.version
        expected_module_count = @($manifest.modules).Count
        program_path = $programPath
        module_paths = $modulePaths.ToArray()
        all_paths = $allPaths.ToArray()
        artifacts = $rows.ToArray()
        errors = $errors.ToArray()
        warnings = $warnings.ToArray()
    }
}

Export-ModuleMember -Function `
    Get-CalcEmuLargestArtifactPath, `
    Get-CalcEmuTransferManifestArtifacts, `
    Resolve-CalcEmuOfficialSplitSuiteArtifacts
