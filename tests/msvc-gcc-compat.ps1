$ErrorActionPreference = 'Stop'

function Resolve-VcVarsPath {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $fallback = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $fallback) {
        return $fallback
    }

    throw 'Could not locate vcvars64.bat'
}

function Quote-CmdArg {
    param([string]$Value)

    if ($Value -match '"') {
        return $Value
    }
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Normalize-MsvcPath {
    param([string]$Value)

    if ($null -eq $Value) {
        return $Value
    }
    return ($Value -replace '/', '\')
}

$rawArgs = $env:MSVC_GCC_ARGS
if (-not $rawArgs) {
    throw 'MSVC_GCC_ARGS was empty'
}

$CompilerArgs = $rawArgs -split '\s+'
$translated = New-Object System.Collections.Generic.List[string]
$translated.Add('/nologo')
$translated.Add('/TC')
$translated.Add('/D_CRT_SECURE_NO_WARNINGS')

for ($i = 0; $i -lt $CompilerArgs.Count; $i++) {
    $arg = $CompilerArgs[$i]

    switch -Regex ($arg) {
        '^-Wall$' { continue }
        '^-Wextra$' { $translated.Add('/W4'); continue }
        '^-Werror$' { $translated.Add('/WX'); continue }
        '^-Wno-unused-function$' { $translated.Add('/wd4505'); continue }
        '^-g$' { $translated.Add('/Zi'); continue }
        '^-O0$' { $translated.Add('/Od'); continue }
        '^-std=c99$' { $translated.Add('/std:c11'); continue }
        '^-lm$' { continue }
        '^-o$' {
            $i++
            if ($i -ge $CompilerArgs.Count) {
                throw 'Missing output file after -o'
            }
            $translated.Add('/Fe:' + (Normalize-MsvcPath $CompilerArgs[$i]))
            continue
        }
        '^-include$' {
            # gcc's forced include (make -C tests run-ce-fmt) is /FI in MSVC.
            $i++
            if ($i -ge $CompilerArgs.Count) {
                throw 'Missing header after -include'
            }
            $translated.Add('/FI"' + (Normalize-MsvcPath $CompilerArgs[$i]) + '"')
            continue
        }
        '^-I(.+)$' {
            $translated.Add('/I"' + (Normalize-MsvcPath $Matches[1]) + '"')
            continue
        }
        '^-D(.+)$' {
            $translated.Add('/D' + $Matches[1])
            continue
        }
        default {
            if ($arg.StartsWith('-')) {
                continue
            }
            $translated.Add((Normalize-MsvcPath $arg))
        }
    }
}

$vcvars = Resolve-VcVarsPath
$translatedArgs = ($translated | ForEach-Object { Quote-CmdArg $_ }) -join ' '
$cmd = 'call "{0}" >nul && cl {1}' -f $vcvars, $translatedArgs

& cmd.exe /c $cmd
exit $LASTEXITCODE
