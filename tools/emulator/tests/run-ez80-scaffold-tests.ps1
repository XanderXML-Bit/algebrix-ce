param(
    [string]$ContractPath = "tools/emulator/ez80/tests/ez80.scaffold.contract.json",
    [switch]$SkipRuntime
)

$ErrorActionPreference = "Stop"

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "FAIL: $Message"
    }
}

function Resolve-RepoPath {
    param([string]$RelativePath)
    $resolved = Join-Path (Get-Location) $RelativePath
    return [System.IO.Path]::GetFullPath($resolved)
}

Write-Host "=== eZ80 Scaffold Contract Tests ==="

$contractAbs = Resolve-RepoPath $ContractPath
Assert-True (Test-Path $contractAbs) "Contract file exists: $ContractPath"

$contract = Get-Content -Raw -Path $contractAbs | ConvertFrom-Json
Assert-True ($null -ne $contract.required_files) "Contract has required_files"
Assert-True ($null -ne $contract.required_symbols) "Contract has required_symbols"

foreach ($rel in $contract.required_files) {
    $abs = Resolve-RepoPath $rel
    Assert-True (Test-Path $abs) "Required file exists: $rel"
}

foreach ($entry in $contract.required_symbols) {
    $rel = $entry.file
    $abs = Resolve-RepoPath $rel
    Assert-True (Test-Path $abs) "Symbol-check file exists: $rel"
    $text = Get-Content -Raw -Path $abs
    foreach ($pat in $entry.patterns) {
        Assert-True ($text.Contains($pat)) "$rel contains pattern: $pat"
    }
}

Write-Host "PASS: eZ80 scaffold contract satisfied."

if (-not $SkipRuntime) {
    $runtimeScript = Resolve-RepoPath "tools/emulator/tests/run-ez80-scaffold-runtime-tests.ps1"
    Assert-True (Test-Path $runtimeScript) "Runtime scaffold test script exists"

    $powershellPath = "powershell.exe"
    $runtimeArgs = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $runtimeScript
    )

    & $powershellPath @runtimeArgs
    Assert-True ($LASTEXITCODE -eq 0) "Runtime scaffold tests pass"
}
