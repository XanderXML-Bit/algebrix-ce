param(
    [string]$Config = "tests/cemu/ci-config/cemu-validate-config.json",
    [string]$Mode = "full",
    [string]$Output = "tests/cemu/outputs/cemu-pipeline-summary.json",
    [string]$Snapshot = "tests/cemu/outputs/cemu-pipeline-snapshot.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $repoRoot "tests/cemu/run_cemu_validation.py"

function Resolve-CalcEnginePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,

        [Parameter(Mandatory = $false)]
        [bool]$RequireExists = $true
    )

    $candidate = $PathValue
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $repoRoot $candidate
    }

    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if ($RequireExists -and -not (Test-Path -LiteralPath $resolved)) {
        throw "Path '$resolved' does not exist."
    }

    return $resolved
}

function Resolve-PythonLaunchCommand {
    $pythonCommand = Get-Command -Name "python.exe" -ErrorAction SilentlyContinue
    if ($null -eq $pythonCommand) {
        $pythonCommand = Get-Command -Name "python" -ErrorAction SilentlyContinue
    }
    if ($null -ne $pythonCommand) {
        return [string[]]@($pythonCommand.Source)
    }

    $pyCommand = Get-Command -Name "py.exe" -ErrorAction SilentlyContinue
    if ($null -eq $pyCommand) {
        $pyCommand = Get-Command -Name "py" -ErrorAction SilentlyContinue
    }
    if ($null -ne $pyCommand) {
        return [string[]]@($pyCommand.Source, "-3")
    }

    throw "Python 3 launcher was not found. Install Python or ensure python.exe/py.exe is on PATH."
}

$resolvedConfig = Resolve-CalcEnginePath -PathValue $Config -RequireExists $true
$resolvedOutput = Resolve-CalcEnginePath -PathValue $Output -RequireExists $false
$resolvedSnapshot = Resolve-CalcEnginePath -PathValue $Snapshot -RequireExists $false
$pythonCommand = @(Resolve-PythonLaunchCommand)
$pythonExecutable = $pythonCommand[0]
$pythonArguments = @($pythonCommand | Select-Object -Skip 1)

& $pythonExecutable @pythonArguments $runner --config $resolvedConfig --mode $Mode --output $resolvedOutput --snapshot $resolvedSnapshot
$exitCode = $LASTEXITCODE
exit $exitCode
