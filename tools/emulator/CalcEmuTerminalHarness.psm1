Set-StrictMode -Version Latest

$artifactModulePath = Join-Path $PSScriptRoot "CalcEmuSplitSuiteArtifacts.psm1"
if (-not (Test-Path -LiteralPath $artifactModulePath -PathType Leaf)) {
    throw "Artifact discovery module '$artifactModulePath' does not exist."
}
Import-Module -Name $artifactModulePath -Force

function Get-CalcEmuKeyAliasMap {
    $aliasMap = @{}

    for ($digit = 0; $digit -le 9; $digit++) {
        $canonical = "KEY_$digit"
        $textDigit = [string]$digit
        $aliasMap[$textDigit] = $canonical
        $aliasMap["key_$digit"] = $canonical
        $aliasMap["digit$digit"] = $canonical
    }

    $aliasSets = @(
        @{ Canonical = "KEY_ADD"; Aliases = @("+", "add", "plus", "key_add") },
        @{ Canonical = "KEY_SUB"; Aliases = @("-", "sub", "minus", "subtract", "key_sub") },
        @{ Canonical = "KEY_MUL"; Aliases = @("*", "x", "mul", "times", "key_mul") },
        @{ Canonical = "KEY_DIV"; Aliases = @("/", "div", "divide", "key_div") },
        @{ Canonical = "KEY_POWER"; Aliases = @("^", "pow", "power", "key_power") },
        @{ Canonical = "KEY_DECIMAL"; Aliases = @(".", "decimal", "dot", "point", "key_decimal") },
        @{ Canonical = "KEY_ENTER"; Aliases = @("enter", "return", "key_enter") },
        @{ Canonical = "KEY_CLEAR"; Aliases = @("clear", "clr", "key_clear") },
        @{ Canonical = "KEY_MODE"; Aliases = @("mode", "key_mode") },
        @{ Canonical = "KEY_YEQU"; Aliases = @("y=", "yequ", "key_yequ") },
        @{ Canonical = "KEY_WINDOW"; Aliases = @("window", "key_window") },
        @{ Canonical = "KEY_ZOOM"; Aliases = @("zoom", "key_zoom") },
        @{ Canonical = "KEY_TRACE"; Aliases = @("trace", "key_trace") },
        @{ Canonical = "KEY_GRAPH"; Aliases = @("graph", "key_graph") },
        @{ Canonical = "KEY_NEG"; Aliases = @("neg", "negative", "key_neg") },
        @{ Canonical = "KEY_LPAREN"; Aliases = @("(", "lparen", "leftparen", "key_lparen") },
        @{ Canonical = "KEY_RPAREN"; Aliases = @(")", "rparen", "rightparen", "key_rparen") },
        @{ Canonical = "KEY_2ND"; Aliases = @("2nd", "second", "key_2nd") },
        @{ Canonical = "KEY_APPS"; Aliases = @("apps", "key_apps") },
        @{ Canonical = "KEY_ALPHA"; Aliases = @("alpha", "key_alpha") },
        @{ Canonical = "KEY_MATH"; Aliases = @("math", "key_math") },
        @{ Canonical = "KEY_ON"; Aliases = @("on", "break", "key_on") },
        @{ Canonical = "KEY_SIN"; Aliases = @("sin", "key_sin") },
        @{ Canonical = "KEY_COS"; Aliases = @("cos", "key_cos") },
        @{ Canonical = "KEY_TAN"; Aliases = @("tan", "key_tan") },
        @{ Canonical = "KEY_LN"; Aliases = @("ln", "key_ln") },
        @{ Canonical = "KEY_LOG"; Aliases = @("log", "key_log") },
        @{ Canonical = "KEY_SQUARE"; Aliases = @("x^2", "square", "key_square") },
        @{ Canonical = "KEY_RECIP"; Aliases = @("x^-1", "recip", "reciprocal", "key_recip") },
        @{ Canonical = "KEY_DEL"; Aliases = @("del", "delete", "key_del") },
        @{ Canonical = "KEY_UP"; Aliases = @("up", "uparrow", "key_up") },
        @{ Canonical = "KEY_DOWN"; Aliases = @("down", "downarrow", "key_down") },
        @{ Canonical = "KEY_LEFT"; Aliases = @("left", "leftarrow", "key_left") },
        @{ Canonical = "KEY_RIGHT"; Aliases = @("right", "rightarrow", "key_right") },
        @{ Canonical = "KEY_STAT"; Aliases = @("stat", "key_stat") },
        @{ Canonical = "KEY_VARS"; Aliases = @("vars", "key_vars") },
        @{ Canonical = "KEY_XVAR"; Aliases = @("xvar", "x,t,theta,n", "xttn", "key_xvar") },
        @{ Canonical = "KEY_COMMA"; Aliases = @(",", "comma", "key_comma") }
    )

    foreach ($aliasSet in $aliasSets) {
        foreach ($alias in $aliasSet.Aliases) {
            $aliasMap[$alias] = $aliasSet.Canonical
        }
    }

    return $aliasMap
}

function ConvertTo-CalcEmuKeySequence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$KeySequence
    )

    if ([string]::IsNullOrWhiteSpace($KeySequence)) {
        throw "KeySequence cannot be empty. Pass a comma-separated list like '2,+,3,enter'."
    }

    $aliasMap = Get-CalcEmuKeyAliasMap
    $supportedCanonicalKeys = New-Object System.Collections.Generic.HashSet[string] ([System.StringComparer]::Ordinal)
    foreach ($canonicalKey in $aliasMap.Values) {
        [void]$supportedCanonicalKeys.Add([string]$canonicalKey)
    }

    $tokens = $KeySequence -split ","
    $normalized = New-Object System.Collections.Generic.List[string]

    foreach ($token in $tokens) {
        $trimmed = $token.Trim()
        if ($trimmed.Length -eq 0) {
            continue
        }

        $upper = $trimmed.ToUpperInvariant()
        if ($upper -match "^KEY_[A-Z0-9_]+$") {
            if (-not $supportedCanonicalKeys.Contains($upper)) {
                throw "Unknown key token '$trimmed'. Use canonical KEY_* names or aliases like '2,+,3,enter'."
            }

            $normalized.Add($upper)
            continue
        }

        $lookup = $trimmed.ToLowerInvariant()
        if ($aliasMap.ContainsKey($lookup)) {
            $normalized.Add($aliasMap[$lookup])
            continue
        }

        throw "Unknown key token '$trimmed'. Use canonical KEY_* names or aliases like '2,+,3,enter'."
    }

    if ($normalized.Count -eq 0) {
        throw "No valid key tokens were parsed from KeySequence '$KeySequence'."
    }

    return ,$normalized.ToArray()
}

function ConvertTo-CalcEmuExpressionToken {
    param(
        [Parameter(Mandatory = $true)]
        [string]$NormalizedKey
    )

    if ($NormalizedKey -match "^KEY_[0-9]$") {
        return $NormalizedKey.Substring(4, 1)
    }

    switch ($NormalizedKey) {
        "KEY_ADD" { return "+" }
        "KEY_SUB" { return "-" }
        "KEY_MUL" { return "*" }
        "KEY_DIV" { return "/" }
        "KEY_POWER" { return "^" }
        "KEY_DECIMAL" { return "." }
        "KEY_LPAREN" { return "(" }
        "KEY_RPAREN" { return ")" }
        "KEY_NEG" { return "-" }
        "KEY_COMMA" { return "," }
        "KEY_SIN" { return "sin(" }
        "KEY_COS" { return "cos(" }
        "KEY_TAN" { return "tan(" }
        "KEY_LN" { return "ln(" }
        "KEY_LOG" { return "log(" }
        "KEY_SQUARE" { return "^2" }
        "KEY_XVAR" { return "x" }
        default { return $null }
    }
}

function ConvertTo-CalcEmuAutotestKeyToken {
    param(
        [Parameter(Mandatory = $true)]
        [string]$NormalizedKey
    )

    if ($NormalizedKey -match "^KEY_[0-9]$") {
        return $NormalizedKey.Substring(4, 1)
    }

    switch ($NormalizedKey) {
        "KEY_ADD" { return "+" }
        "KEY_SUB" { return "-" }
        "KEY_MUL" { return "*" }
        "KEY_DIV" { return "/" }
        "KEY_POWER" { return "^" }
        "KEY_DECIMAL" { return "." }
        "KEY_ENTER" { return "enter" }
        "KEY_CLEAR" { return "clear" }
        "KEY_MODE" { return "mode" }
        "KEY_YEQU" { return "yequ" }
        "KEY_WINDOW" { return "window" }
        "KEY_ZOOM" { return "zoom" }
        "KEY_TRACE" { return "trace" }
        "KEY_GRAPH" { return "graph" }
        "KEY_NEG" { return "(-)" }
        "KEY_LPAREN" { return "(" }
        "KEY_RPAREN" { return ")" }
        "KEY_2ND" { return "2nd" }
        "KEY_APPS" { return "apps" }
        "KEY_ALPHA" { return "alpha" }
        "KEY_MATH" { return "math" }
        "KEY_ON" { return "on" }
        "KEY_SIN" { return "sin" }
        "KEY_COS" { return "cos" }
        "KEY_TAN" { return "tan" }
        "KEY_LN" { return "ln" }
        "KEY_LOG" { return "log" }
        "KEY_SQUARE" { return "square" }
        "KEY_RECIP" { return "recip" }
        "KEY_DEL" { return "del" }
        "KEY_UP" { return "up" }
        "KEY_DOWN" { return "down" }
        "KEY_LEFT" { return "left" }
        "KEY_RIGHT" { return "right" }
        "KEY_STAT" { return "stat" }
        "KEY_VARS" { return "vars" }
        "KEY_XVAR" { return "xvar" }
        "KEY_COMMA" { return "," }
        default {
            throw "Normalized key '$NormalizedKey' does not have an upstream CEmu AutoTester mapping."
        }
    }
}

function Get-CalcEmuOperatorPrecedence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OperatorToken
    )

    switch ($OperatorToken) {
        "u+" { return 3 }
        "u-" { return 3 }
        "^" { return 3 }
        "*" { return 2 }
        "/" { return 2 }
        "+" { return 1 }
        "-" { return 1 }
        default { throw "Unknown operator token '$OperatorToken'." }
    }
}

function Test-CalcEmuRightAssociativeOperator {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OperatorToken
    )

    if ($OperatorToken -eq "^" -or $OperatorToken -eq "u+" -or $OperatorToken -eq "u-") {
        return $true
    }

    return $false
}

function Split-CalcEmuExpressionTokens {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Expression
    )

    $tokens = New-Object System.Collections.Generic.List[string]
    $length = $Expression.Length
    $position = 0

    while ($position -lt $length) {
        $char = $Expression[$position]

        if ([char]::IsWhiteSpace($char)) {
            $position++
            continue
        }

        if (($char -ge '0' -and $char -le '9') -or $char -eq '.') {
            $start = $position
            $dotCount = 0
            $digitCount = 0

            while ($position -lt $length) {
                $scan = $Expression[$position]
                if ($scan -ge '0' -and $scan -le '9') {
                    $digitCount++
                    $position++
                    continue
                }

                if ($scan -eq '.') {
                    $dotCount++
                    if ($dotCount -gt 1) {
                        throw "Invalid number near position $position in expression '$Expression'."
                    }

                    $position++
                    continue
                }

                break
            }

            if ($digitCount -eq 0) {
                throw "Invalid number near position $start in expression '$Expression'."
            }

            $token = $Expression.Substring($start, $position - $start)
            $tokens.Add($token)
            continue
        }

        if ($char -eq '+' -or $char -eq '-' -or $char -eq '*' -or $char -eq '/' -or $char -eq '^' -or $char -eq '(' -or $char -eq ')') {
            $tokens.Add([string]$char)
            $position++
            continue
        }

        throw "Unsupported expression character '$char' at position $position."
    }

    return ,$tokens.ToArray()
}

function ConvertTo-CalcEmuRpn {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Tokens
    )

    $output = New-Object System.Collections.Generic.List[string]
    $operators = New-Object System.Collections.Generic.Stack[string]
    $expectUnary = $true

    foreach ($token in $Tokens) {
        $number = 0.0
        if ([double]::TryParse($token, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
            $output.Add($token)
            $expectUnary = $false
            continue
        }

        if ($token -eq "(") {
            $operators.Push($token)
            $expectUnary = $true
            continue
        }

        if ($token -eq ")") {
            $foundLeftParen = $false
            while ($operators.Count -gt 0) {
                $top = $operators.Pop()
                if ($top -eq "(") {
                    $foundLeftParen = $true
                    break
                }

                $output.Add($top)
            }

            if (-not $foundLeftParen) {
                throw "Mismatched parenthesis: missing '(' in expression."
            }

            $expectUnary = $false
            continue
        }

        if ($token -ne "+" -and $token -ne "-" -and $token -ne "*" -and $token -ne "/" -and $token -ne "^") {
            throw "Unsupported token '$token' while building expression."
        }

        $operatorToken = $token
        if (($token -eq "+" -or $token -eq "-") -and $expectUnary) {
            $operatorToken = "u$token"
        }

        $currentPrecedence = Get-CalcEmuOperatorPrecedence -OperatorToken $operatorToken
        $currentRightAssociative = Test-CalcEmuRightAssociativeOperator -OperatorToken $operatorToken

        while ($operators.Count -gt 0) {
            $top = $operators.Peek()
            if ($top -eq "(") {
                break
            }

            $topPrecedence = Get-CalcEmuOperatorPrecedence -OperatorToken $top
            $shouldPop = $false

            if ($currentRightAssociative) {
                if ($currentPrecedence -lt $topPrecedence) {
                    $shouldPop = $true
                }
            }
            else {
                if ($currentPrecedence -le $topPrecedence) {
                    $shouldPop = $true
                }
            }

            if (-not $shouldPop) {
                break
            }

            $output.Add($operators.Pop())
        }

        $operators.Push($operatorToken)
        $expectUnary = $true
    }

    while ($operators.Count -gt 0) {
        $top = $operators.Pop()
        if ($top -eq "(") {
            throw "Mismatched parenthesis: missing ')' in expression."
        }

        $output.Add($top)
    }

    return ,$output.ToArray()
}

function Invoke-CalcEmuRpnEvaluation {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$RpnTokens
    )

    $stack = New-Object System.Collections.Generic.Stack[double]

    foreach ($token in $RpnTokens) {
        $number = 0.0
        if ([double]::TryParse($token, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
            $stack.Push($number)
            continue
        }

        if ($token -eq "u+" -or $token -eq "u-") {
            if ($stack.Count -lt 1) {
                throw "Expression is incomplete: unary operator '$token' is missing an operand."
            }

            $value = $stack.Pop()
            if ($token -eq "u-") {
                $stack.Push(-$value)
            }
            else {
                $stack.Push($value)
            }

            continue
        }

        if ($stack.Count -lt 2) {
            throw "Expression is incomplete: operator '$token' is missing operands."
        }

        $right = $stack.Pop()
        $left = $stack.Pop()
        $computed = 0.0

        switch ($token) {
            "+" { $computed = $left + $right }
            "-" { $computed = $left - $right }
            "*" { $computed = $left * $right }
            "/" {
                if ([Math]::Abs($right) -le 1e-15) {
                    throw "Division by zero in expression."
                }

                $computed = $left / $right
            }
            "^" { $computed = [Math]::Pow($left, $right) }
            default { throw "Unsupported operator '$token' during evaluation." }
        }

        if ([double]::IsNaN($computed) -or [double]::IsInfinity($computed)) {
            throw "Expression evaluated to a non-finite value."
        }

        $stack.Push($computed)
    }

    if ($stack.Count -ne 1) {
        throw "Expression is malformed and could not be evaluated."
    }

    return $stack.Pop()
}

function Format-CalcEmuNumericResult {
    param(
        [Parameter(Mandatory = $true)]
        [double]$Value
    )

    if ([double]::IsNaN($Value)) {
        return "NaN"
    }

    if ([double]::IsPositiveInfinity($Value)) {
        return "Infinity"
    }

    if ([double]::IsNegativeInfinity($Value)) {
        return "-Infinity"
    }

    $adjusted = $Value
    if ([Math]::Abs($adjusted) -lt 5e-15) {
        $adjusted = 0.0
    }

    $result = $adjusted.ToString("G15", [System.Globalization.CultureInfo]::InvariantCulture)
    if ($result -eq "-0") {
        return "0"
    }

    return $result
}

function Invoke-CalcEmuExpressionEvaluation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Expression
    )

    if ([string]::IsNullOrWhiteSpace($Expression)) {
        return [ordered]@{
            status = "pending"
            result = ""
            error = ""
        }
    }

    try {
        $tokens = Split-CalcEmuExpressionTokens -Expression $Expression
        $rpn = ConvertTo-CalcEmuRpn -Tokens $tokens
        $numericResult = Invoke-CalcEmuRpnEvaluation -RpnTokens $rpn
        $formattedResult = Format-CalcEmuNumericResult -Value $numericResult

        return [ordered]@{
            status = "ok"
            result = $formattedResult
            error = ""
        }
    }
    catch {
        return [ordered]@{
            status = "error"
            result = ""
            error = $_.Exception.Message
        }
    }
}

function Get-CalcEmuMockScreenState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$NormalizedKeys
    )

    $expressionBuilder = New-Object System.Text.StringBuilder
    $ignoredKeys = New-Object System.Collections.Generic.List[string]
    $status = "pending"
    $result = ""
    $error = ""

    foreach ($normalizedKey in $NormalizedKeys) {
        if ($normalizedKey -eq "KEY_CLEAR") {
            $expressionBuilder = New-Object System.Text.StringBuilder
            $status = "pending"
            $result = ""
            $error = ""
            continue
        }

        if ($normalizedKey -eq "KEY_ENTER") {
            $evaluation = Invoke-CalcEmuExpressionEvaluation -Expression $expressionBuilder.ToString()
            $status = $evaluation.status
            $result = $evaluation.result
            $error = $evaluation.error
            continue
        }

        $expressionToken = ConvertTo-CalcEmuExpressionToken -NormalizedKey $normalizedKey
        if ($null -ne $expressionToken) {
            [void]$expressionBuilder.Append($expressionToken)
            if ($status -ne "pending") {
                $status = "pending"
                $result = ""
                $error = ""
            }

            continue
        }

        $ignoredKeys.Add($normalizedKey)
    }

    $expression = $expressionBuilder.ToString()
    $expressionText = "<empty>"
    if (-not [string]::IsNullOrWhiteSpace($expression)) {
        $expressionText = $expression
    }

    $resultText = "<none>"
    if ($status -eq "ok") {
        $resultText = $result
    }
    elseif ($status -eq "error") {
        $resultText = "<error>"
    }

    $errorText = "<none>"
    if (-not [string]::IsNullOrWhiteSpace($error)) {
        $errorText = $error
    }

    $ignoredText = "<none>"
    if ($ignoredKeys.Count -gt 0) {
        $ignoredText = ($ignoredKeys.ToArray() -join ",")
    }

    $lines = @(
        "EXPR: $expressionText",
        "RESULT: $resultText",
        "STATUS: $status",
        "ERROR: $errorText",
        "IGNORED_KEYS: $ignoredText"
    )

    return [ordered]@{
        expression = $expression
        result = $result
        status = $status
        error = $error
        ignored_keys = $ignoredKeys.ToArray()
        lines = $lines
        text = ($lines -join "`n")
    }
}

function Resolve-CalcEmuRepoRoot {
    $moduleRoot = Split-Path -Path $PSScriptRoot -Parent
    return (Resolve-Path -LiteralPath (Join-Path $moduleRoot "..")).ProviderPath
}

function Get-CalcEmuAutotestReadiness {
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$RomPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$AutotesterPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ProgramPath
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    $repoRoot = Resolve-CalcEmuRepoRoot
    $wrapperPath = Join-Path $repoRoot "tools\emulator\run-cemu-autotest.ps1"

    if (-not (Test-Path -LiteralPath $wrapperPath -PathType Leaf)) {
        $reasons.Add("Missing existing CEmu wrapper script at '$wrapperPath'.")
    }

    $resolvedRomPath = ""
    if ([string]::IsNullOrWhiteSpace($RomPath)) {
        $reasons.Add("RomPath is required for CEmu mode (pass -RomPath or set CEMU_ROM_PATH).")
    }
    elseif (-not (Test-Path -LiteralPath $RomPath -PathType Leaf)) {
        $reasons.Add("RomPath '$RomPath' does not exist.")
    }
    else {
        $resolvedRomPath = (Resolve-Path -LiteralPath $RomPath).ProviderPath
    }

    $resolvedAutotesterPath = ""
    if ([string]::IsNullOrWhiteSpace($AutotesterPath)) {
        $reasons.Add("AutotesterPath is required for CEmu mode (pass -AutotesterPath or set CEMU_AUTOTESTER_PATH).")
    }
    elseif (-not (Test-Path -LiteralPath $AutotesterPath -PathType Leaf)) {
        $reasons.Add("AutotesterPath '$AutotesterPath' does not exist.")
    }
    else {
        $resolvedAutotesterPath = (Resolve-Path -LiteralPath $AutotesterPath).ProviderPath
    }

    $programCandidate = $ProgramPath
    if ([string]::IsNullOrWhiteSpace($programCandidate)) {
        $officialArtifacts = $null
        try {
            $officialArtifacts = Resolve-CalcEmuOfficialSplitSuiteArtifacts -RepoRoot $repoRoot
        }
        catch {
            $officialArtifacts = $null
        }

        if ($null -ne $officialArtifacts -and -not [string]::IsNullOrWhiteSpace([string]$officialArtifacts.program_path)) {
            $programCandidate = [string]$officialArtifacts.program_path
        }
        else {
            $fallbackCandidates = @(
                (Join-Path $repoRoot "bin\launcher\CALCENG.8xp"),
                (Join-Path $repoRoot "bin\CALCENG.8xp")
            )
            foreach ($fallbackCandidate in $fallbackCandidates) {
                if (Test-Path -LiteralPath $fallbackCandidate -PathType Leaf) {
                    $programCandidate = $fallbackCandidate
                    break
                }
            }
        }
    }

    $resolvedProgramPath = ""
    if ([string]::IsNullOrWhiteSpace($programCandidate)) {
        $reasons.Add("ProgramPath is required for CEmu mode (build CalcEngine or pass -ProgramPath).")
    }
    elseif (-not (Test-Path -LiteralPath $programCandidate -PathType Leaf)) {
        $reasons.Add("ProgramPath '$programCandidate' does not exist. Build CalcEngine or pass -ProgramPath.")
    }
    else {
        $resolvedProgramPath = (Resolve-Path -LiteralPath $programCandidate).ProviderPath
    }

    return [ordered]@{
        can_run = ($reasons.Count -eq 0)
        reasons = $reasons.ToArray()
        wrapper_path = $wrapperPath
        rom_path = $resolvedRomPath
        autotester_path = $resolvedAutotesterPath
        program_path = $resolvedProgramPath
    }
}

function Get-CalcEmuHardwareConstraintValidation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ModuleProgramPaths = @(),

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$UsableUserRamLimitBytes = 65536,

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$ModuleLinkedBinaryLimitBytes = 51200
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $moduleRows = New-Object System.Collections.Generic.List[object]
    $moduleSeen = @{}
    $moduleOverLimitCount = 0
    $moduleMissingCount = 0

    $programValidation = [ordered]@{
        checked = $false
        path = $ProgramPath
        resolved_path = ""
        exists = $false
        size_bytes = $null
        limit_bytes = $UsableUserRamLimitBytes
        within_limit = $null
        memory_insufficient = $false
        error = ""
    }

    if (-not [string]::IsNullOrWhiteSpace($ProgramPath)) {
        $programValidation.checked = $true
        if (-not (Test-Path -LiteralPath $ProgramPath -PathType Leaf)) {
            $programValidation.exists = $false
            $programValidation.within_limit = $false
            $programValidation.error = "ProgramPath '$ProgramPath' does not exist."
            $errors.Add($programValidation.error)
        }
        else {
            $resolvedProgramPath = (Resolve-Path -LiteralPath $ProgramPath).ProviderPath
            $programValidation.exists = $true
            $programValidation.resolved_path = $resolvedProgramPath
            $programValidation.size_bytes = [int64](Get-Item -LiteralPath $resolvedProgramPath).Length
            $programValidation.within_limit = ($programValidation.size_bytes -le $UsableUserRamLimitBytes)

            if (-not $programValidation.within_limit) {
                $programValidation.memory_insufficient = $true
                $programValidation.error = (
                    "Memory is insufficient to perform the calculation. Program '{0}' is {1} bytes and exceeds the usable user RAM limit of {2} bytes (~64KB)." -f
                    $resolvedProgramPath, $programValidation.size_bytes, $UsableUserRamLimitBytes
                )
                $errors.Add($programValidation.error)
            }
        }
    }

    foreach ($moduleProgramPath in @($ModuleProgramPaths)) {
        if ($null -eq $moduleProgramPath) {
            $errors.Add("ModuleProgramPaths cannot contain null values.")
            continue
        }

        $rawPath = [string]$moduleProgramPath
        $trimmedPath = $rawPath.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmedPath)) {
            $errors.Add("ModuleProgramPaths cannot contain empty values.")
            continue
        }

        $dedupeKey = $trimmedPath.ToLowerInvariant()
        if ($moduleSeen.ContainsKey($dedupeKey)) {
            continue
        }
        $moduleSeen[$dedupeKey] = $true

        $moduleRow = [ordered]@{
            path = $trimmedPath
            resolved_path = ""
            exists = $false
            size_bytes = $null
            limit_bytes = $ModuleLinkedBinaryLimitBytes
            within_limit = $null
            error = ""
        }

        if (-not (Test-Path -LiteralPath $trimmedPath -PathType Leaf)) {
            $moduleRow.exists = $false
            $moduleRow.within_limit = $false
            $moduleRow.error = "Module program path '$trimmedPath' does not exist."
            $moduleMissingCount++
            $errors.Add($moduleRow.error)
            $moduleRows.Add($moduleRow)
            continue
        }

        $resolvedModulePath = (Resolve-Path -LiteralPath $trimmedPath).ProviderPath
        $moduleRow.resolved_path = $resolvedModulePath
        $moduleRow.exists = $true
        $moduleRow.size_bytes = [int64](Get-Item -LiteralPath $resolvedModulePath).Length
        $moduleRow.within_limit = ($moduleRow.size_bytes -le $ModuleLinkedBinaryLimitBytes)

        if (-not $moduleRow.within_limit) {
            $moduleRow.error = (
                "Module linked binary exceeds limit. Module '{0}' is {1} bytes and must be <= {2} bytes (~50KB)." -f
                $resolvedModulePath, $moduleRow.size_bytes, $ModuleLinkedBinaryLimitBytes
            )
            $moduleOverLimitCount++
            $errors.Add($moduleRow.error)
        }

        $moduleRows.Add($moduleRow)
    }

    $validated = ($programValidation.checked -or $moduleRows.Count -gt 0 -or $ModuleProgramPaths.Count -gt 0)
    $validationPass = $true
    if ($validated) {
        $validationPass = ($errors.Count -eq 0)
    }

    return [ordered]@{
        schema_version = 1
        validated = $validated
        pass = $validationPass
        memory_insufficient = [bool]$programValidation.memory_insufficient
        module_size_exceeded = ($moduleOverLimitCount -gt 0)
        usable_user_ram_limit_bytes = $UsableUserRamLimitBytes
        module_linked_binary_limit_bytes = $ModuleLinkedBinaryLimitBytes
        program = $programValidation
        modules = [ordered]@{
            checked_count = $moduleRows.Count
            over_limit_count = $moduleOverLimitCount
            missing_count = $moduleMissingCount
            items = $moduleRows.ToArray()
        }
        errors = $errors.ToArray()
    }
}

function Get-CalcEmuSha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha256.ComputeHash($Bytes)
        return ([System.BitConverter]::ToString($hash)).Replace("-", "")
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-CalcEmuArchiveRamSwapSimulation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$DispatchModulePath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ModulePaths = @(),

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$SwapRamCapacityBytes = 65536,

        [Parameter(Mandatory = $false)]
        [ValidateSet("none", "dispatch_load", "return_to_archive", "partial_load", "memory_pressure", "archive_full")]
        [string]$FailurePhase = "none",

        [Parameter(Mandatory = $false)]
        [ValidateRange(1, 256)]
        [int]$RapidSwapCycles = 1,

        [Parameter(Mandatory = $false)]
        [ValidateRange(0, 1048576)]
        [int]$ReservedRamBytes = 0,

        [Parameter(Mandatory = $false)]
        [switch]$SimulatePartialLoad
    )

    $normalizedPaths = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    if (-not [string]::IsNullOrWhiteSpace($DispatchModulePath)) {
        $dispatchTrimmed = $DispatchModulePath.Trim()
        if ($dispatchTrimmed.Length -gt 0) {
            $dispatchKey = $dispatchTrimmed.ToLowerInvariant()
            if (-not $seen.ContainsKey($dispatchKey)) {
                $seen[$dispatchKey] = $true
                $normalizedPaths.Add($dispatchTrimmed)
            }
        }
    }

    foreach ($modulePath in @($ModulePaths)) {
        if ($null -eq $modulePath) {
            continue
        }

        $trimmedPath = ([string]$modulePath).Trim()
        if ($trimmedPath.Length -eq 0) {
            continue
        }

        $dedupeKey = $trimmedPath.ToLowerInvariant()
        if ($seen.ContainsKey($dedupeKey)) {
            continue
        }

        $seen[$dedupeKey] = $true
        $normalizedPaths.Add($trimmedPath)
    }

    $simulationRequested = ($normalizedPaths.Count -gt 0)
    $diagnostics = New-Object System.Collections.Generic.List[string]
    $errors = New-Object System.Collections.Generic.List[string]
    $moduleRows = New-Object System.Collections.Generic.List[object]

    $dispatch = [ordered]@{
        requested = $simulationRequested
        module_path = ""
        resolved_module_path = ""
        module_size_bytes = $null
        loaded_to_ram = $false
        error = ""
    }

    $returnToArchive = [ordered]@{
        attempted = $false
        returned_to_archive = $false
        error = ""
    }

    $effectiveFailurePhase = "none"
    $pass = $true
    $moduleInArchive = $true
    $moduleInRam = $false
    $dispatchEvents = 0
    $returnEvents = 0
    $moduleInRamCount = 0
    $corruptionDetected = $false
    $partialLoadDetected = $false
    $dispatchFailureInjected = $false
    $returnFailureInjected = $false
    $partialFailureInjected = $false
    $effectiveRamCapacityBytes = $SwapRamCapacityBytes - $ReservedRamBytes

    if ($effectiveRamCapacityBytes -lt 0) {
        $effectiveRamCapacityBytes = 0
    }

    if ($FailurePhase -eq "memory_pressure") {
        $effectiveRamCapacityBytes = 0
    }

    if (-not $simulationRequested) {
        if ($FailurePhase -ne "none" -or $SimulatePartialLoad.IsPresent) {
            $pass = $false
            $effectiveFailurePhase = "dispatch_load"
            $message = "Archive<->RAM swap simulation requested fault behavior but no module path was provided."
            $errors.Add($message)
            $diagnostics.Add($message)
        }
        else {
            $diagnostics.Add("Archive<->RAM swap simulation was not requested.")
        }

        return [ordered]@{
            schema_version = 1
            simulated = $simulationRequested
            pass = $pass
            failure_phase = $effectiveFailurePhase
            ram_capacity_bytes = $SwapRamCapacityBytes
            dispatch = $dispatch
            return_to_archive = $returnToArchive
            state = [ordered]@{
                module_in_archive = $moduleInArchive
                module_in_ram = $moduleInRam
            }
            modules = [ordered]@{
                requested_count = 0
                rapid_swap_cycles = $RapidSwapCycles
                reserved_ram_bytes = $ReservedRamBytes
                effective_ram_capacity_bytes = $effectiveRamCapacityBytes
                dispatch_events = 0
                return_events = 0
                corruption_detected = $false
                partial_load_detected = $false
                items = @()
            }
            diagnostics = $diagnostics.ToArray()
            errors = $errors.ToArray()
        }
    }

    $dispatch.module_path = [string]$normalizedPaths[0]
    $diagnostics.Add(("Archive<->RAM simulation requested for {0} module(s) across {1} cycle(s)." -f $normalizedPaths.Count, $RapidSwapCycles))
    $diagnostics.Add(("Swap RAM capacity={0} bytes, reserved={1} bytes, effective={2} bytes." -f $SwapRamCapacityBytes, $ReservedRamBytes, $effectiveRamCapacityBytes))

    for ($cycleIndex = 1; $cycleIndex -le $RapidSwapCycles; $cycleIndex++) {
        foreach ($modulePath in $normalizedPaths) {
            $moduleRow = [ordered]@{
                cycle = $cycleIndex
                path = $modulePath
                resolved_path = ""
                exists = $false
                size_bytes = $null
                checksum_before = ""
                checksum_after = ""
                loaded_to_ram = $false
                returned_to_archive = $false
                partial_load_detected = $false
                corruption_detected = $false
                error = ""
            }

            if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
                $moduleRow.error = "Archive<->RAM dispatch failed: module path '$modulePath' does not exist."
                $pass = $false
                if ($effectiveFailurePhase -eq "none") {
                    $effectiveFailurePhase = "dispatch_load"
                }
                if ([string]::IsNullOrWhiteSpace($dispatch.error)) {
                    $dispatch.error = $moduleRow.error
                }
                $errors.Add($moduleRow.error)
                $diagnostics.Add($moduleRow.error)
                $moduleRows.Add($moduleRow)
                continue
            }

            $resolvedModulePath = (Resolve-Path -LiteralPath $modulePath).ProviderPath
            $archiveBytes = [System.IO.File]::ReadAllBytes($resolvedModulePath)
            $moduleSizeBytes = [int64]$archiveBytes.Length

            $moduleRow.resolved_path = $resolvedModulePath
            $moduleRow.exists = $true
            $moduleRow.size_bytes = $moduleSizeBytes
            $moduleRow.checksum_before = Get-CalcEmuSha256Hex -Bytes $archiveBytes

            if ([string]::IsNullOrWhiteSpace($dispatch.resolved_module_path)) {
                $dispatch.resolved_module_path = $resolvedModulePath
                $dispatch.module_size_bytes = $moduleSizeBytes
            }

            if ($moduleSizeBytes -gt $effectiveRamCapacityBytes) {
                $moduleRow.error = (
                    "Archive<->RAM dispatch failed: module '{0}' requires {1} bytes but effective swap RAM capacity is {2} bytes." -f
                    $resolvedModulePath, $moduleSizeBytes, $effectiveRamCapacityBytes
                )
                $pass = $false
                if ($FailurePhase -eq "memory_pressure") {
                    $effectiveFailurePhase = "memory_pressure"
                }
                elseif ($effectiveFailurePhase -eq "none") {
                    $effectiveFailurePhase = "dispatch_load"
                }
                if ([string]::IsNullOrWhiteSpace($dispatch.error)) {
                    $dispatch.error = $moduleRow.error
                }
                $errors.Add($moduleRow.error)
                $diagnostics.Add($moduleRow.error)
                $moduleRows.Add($moduleRow)
                continue
            }

            if ($FailurePhase -eq "dispatch_load" -and -not $dispatchFailureInjected) {
                $dispatchFailureInjected = $true
                $moduleRow.error = "Archive<->RAM dispatch failed: simulated dispatch_load failure was requested."
                $pass = $false
                $effectiveFailurePhase = "dispatch_load"
                if ([string]::IsNullOrWhiteSpace($dispatch.error)) {
                    $dispatch.error = $moduleRow.error
                }
                $errors.Add($moduleRow.error)
                $diagnostics.Add($moduleRow.error)
                $moduleRows.Add($moduleRow)
                continue
            }

            $ramBytes = $archiveBytes
            $partialLoadActive = $false
            if (($SimulatePartialLoad.IsPresent -or $FailurePhase -eq "partial_load") -and -not $partialFailureInjected) {
                $partialFailureInjected = $true
                $partialLoadActive = $true
                $partialLength = [int][Math]::Max(1, $archiveBytes.Length - 1)
                $ramBytes = New-Object byte[] $partialLength
                [System.Array]::Copy($archiveBytes, 0, $ramBytes, 0, $partialLength)
            }

            $moduleRow.loaded_to_ram = $true
            $dispatchEvents++
            $moduleInRamCount++
            $moduleInArchive = $false
            $moduleInRam = $true
            if ($dispatchEvents -eq 1) {
                $dispatch.loaded_to_ram = $true
            }

            if ($partialLoadActive) {
                $partialLoadDetected = $true
                $moduleRow.partial_load_detected = $true
                $moduleRow.corruption_detected = $true
                $corruptionDetected = $true
                $moduleRow.error = (
                    "Archive<->RAM partial load detected: module '{0}' loaded only {1} of {2} bytes." -f
                    $resolvedModulePath, $ramBytes.Length, $archiveBytes.Length
                )
                $pass = $false
                $effectiveFailurePhase = "partial_load"
                if ([string]::IsNullOrWhiteSpace($dispatch.error)) {
                    $dispatch.error = $moduleRow.error
                }
                $errors.Add($moduleRow.error)
                $diagnostics.Add($moduleRow.error)
            }

            $returnToArchive.attempted = $true
            if (($FailurePhase -eq "return_to_archive" -or $FailurePhase -eq "archive_full") -and -not $returnFailureInjected) {
                $returnFailureInjected = $true
                if ($FailurePhase -eq "archive_full") {
                    $moduleRow.error = "RAM->archive return failed: archive is full and the module could not be re-archived."
                    $effectiveFailurePhase = "archive_full"
                }
                else {
                    $moduleRow.error = "RAM->archive return failed: simulated return_to_archive failure was requested."
                    $effectiveFailurePhase = "return_to_archive"
                }
                $pass = $false
                if ([string]::IsNullOrWhiteSpace($returnToArchive.error)) {
                    $returnToArchive.error = $moduleRow.error
                }
                $errors.Add($moduleRow.error)
                $diagnostics.Add($moduleRow.error)
                $moduleRows.Add($moduleRow)
                continue
            }

            $archiveAfterBytes = New-Object byte[] $archiveBytes.Length
            if ($partialLoadActive) {
                [System.Array]::Copy($ramBytes, 0, $archiveAfterBytes, 0, $ramBytes.Length)
            }
            else {
                [System.Array]::Copy($archiveBytes, 0, $archiveAfterBytes, 0, $archiveBytes.Length)
            }

            $moduleRow.checksum_after = Get-CalcEmuSha256Hex -Bytes $archiveAfterBytes
            $moduleRow.returned_to_archive = $true
            $returnEvents++
            if ($returnEvents -eq 1) {
                $returnToArchive.returned_to_archive = $true
            }

            if ($moduleRow.checksum_before -cne $moduleRow.checksum_after) {
                $moduleRow.corruption_detected = $true
                $corruptionDetected = $true
                $pass = $false
                if ($effectiveFailurePhase -eq "none") {
                    $effectiveFailurePhase = "return_to_archive"
                }
                $corruptionMessage = (
                    "Archive<->RAM integrity failure: module '{0}' checksum changed ({1} -> {2})." -f
                    $resolvedModulePath, $moduleRow.checksum_before, $moduleRow.checksum_after
                )
                if ([string]::IsNullOrWhiteSpace($moduleRow.error)) {
                    $moduleRow.error = $corruptionMessage
                }
                if ([string]::IsNullOrWhiteSpace($returnToArchive.error)) {
                    $returnToArchive.error = $corruptionMessage
                }
                $errors.Add($corruptionMessage)
                $diagnostics.Add($corruptionMessage)
            }
            else {
                if ($moduleInRamCount -gt 0) {
                    $moduleInRamCount--
                }
                $moduleInArchive = ($moduleInRamCount -eq 0)
                $moduleInRam = ($moduleInRamCount -gt 0)
                $diagnostics.Add(("Cycle {0}: archive->RAM->archive succeeded for '{1}' ({2} bytes)." -f $cycleIndex, $resolvedModulePath, $moduleSizeBytes))
            }

            $moduleRows.Add($moduleRow)
        }
    }

    if ($returnToArchive.attempted) {
        $returnToArchive.returned_to_archive = (
            $returnEvents -gt 0 -and
            [string]::IsNullOrWhiteSpace($returnToArchive.error) -and
            -not $corruptionDetected -and
            $effectiveFailurePhase -ne "return_to_archive" -and
            $effectiveFailurePhase -ne "archive_full"
        )
    }

    if ($pass) {
        $diagnostics.Add("Archive<->RAM swap simulation passed.")
    }

    return [ordered]@{
        schema_version = 1
        simulated = $simulationRequested
        pass = $pass
        failure_phase = $effectiveFailurePhase
        ram_capacity_bytes = $SwapRamCapacityBytes
        dispatch = $dispatch
        return_to_archive = $returnToArchive
        state = [ordered]@{
            module_in_archive = $moduleInArchive
            module_in_ram = $moduleInRam
        }
        modules = [ordered]@{
            requested_count = $normalizedPaths.Count
            rapid_swap_cycles = $RapidSwapCycles
            reserved_ram_bytes = $ReservedRamBytes
            effective_ram_capacity_bytes = $effectiveRamCapacityBytes
            dispatch_events = $dispatchEvents
            return_events = $returnEvents
            corruption_detected = $corruptionDetected
            partial_load_detected = $partialLoadDetected
            items = $moduleRows.ToArray()
        }
        diagnostics = $diagnostics.ToArray()
        errors = $errors.ToArray()
    }
}

function ConvertTo-CalcEmuExpectedCrcList {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [AllowEmptyCollection()]
        [object[]]$ExpectedVramCrcs
    )

    $normalized = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    foreach ($entry in @($ExpectedVramCrcs)) {
        if ($null -eq $entry) {
            throw "ExpectedVramCrcs cannot contain null values."
        }

        $rawText = [string]$entry
        $trimmed = $rawText.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) {
            throw "ExpectedVramCrcs cannot contain empty values."
        }

        if ($trimmed -notmatch "^(?:0x)?[0-9a-fA-F]{8}$") {
            throw "Invalid CRC value '$trimmed'. CRC values must be 8 hexadecimal characters (optionally prefixed with 0x)."
        }

        if ($trimmed.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
            $trimmed = $trimmed.Substring(2)
        }

        $canonical = $trimmed.ToUpperInvariant()
        if (-not $seen.ContainsKey($canonical)) {
            $seen[$canonical] = $true
            $normalized.Add($canonical)
        }
    }

    return ,$normalized.ToArray()
}

function ConvertTo-CalcEmuCanonicalCrc {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )

    $trimmed = $Value.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return $null
    }

    if ($trimmed.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
        $trimmed = $trimmed.Substring(2)
    }

    if ($trimmed -notmatch "^[0-9a-fA-F]{1,8}$") {
        return $null
    }

    return $trimmed.PadLeft(8, '0').ToUpperInvariant()
}

function Get-CalcEmuHashObservationFromOutput {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$OutputLines,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$HashName = ""
    )

    $normalizedHashName = $HashName
    if ([string]::IsNullOrWhiteSpace($normalizedHashName)) {
        $normalizedHashName = ""
    }

    $observedCrcs = New-Object System.Collections.Generic.List[string]
    $seen = @{}

    foreach ($outputLine in @($OutputLines)) {
        if ($null -eq $outputLine) {
            continue
        }

        $line = [string]$outputLine
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $match = [regex]::Match($line, "Hash\s*#(?<hash>[A-Za-z0-9_]+).*?\bgot\s+(?<crc>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{1,8})\b", [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if (-not $match.Success) {
            continue
        }

        $lineHashName = [string]$match.Groups["hash"].Value
        if (-not [string]::IsNullOrWhiteSpace($normalizedHashName) -and $lineHashName -cne $normalizedHashName) {
            continue
        }

        $canonicalCrc = ConvertTo-CalcEmuCanonicalCrc -Value ([string]$match.Groups["crc"].Value)
        if ($null -eq $canonicalCrc) {
            continue
        }

        if (-not $seen.ContainsKey($canonicalCrc)) {
            $seen[$canonicalCrc] = $true
            $observedCrcs.Add($canonicalCrc)
        }
    }

    $lastObservedCrc = ""
    if ($observedCrcs.Count -gt 0) {
        $lastObservedCrc = $observedCrcs[$observedCrcs.Count - 1]
    }

    return [ordered]@{
        observed_crcs = $observedCrcs.ToArray()
        last_observed_crc = $lastObservedCrc
    }
}

function New-CalcEmuAutotestTemplateFromKeys {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$NormalizedKeys,

        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$HashAssertion = $null
    )

    $sequence = New-Object System.Collections.Generic.List[string]
    $sequence.Add("action|launch")
    $sequence.Add("delay|2500")

    foreach ($normalizedKey in $NormalizedKeys) {
        $autotestKey = ConvertTo-CalcEmuAutotestKeyToken -NormalizedKey $normalizedKey
        $sequence.Add("key|$autotestKey")
    }

    $sequence.Add("delay|300")
    $hashes = [ordered]@{}

    if ($null -ne $HashAssertion -and [bool]$HashAssertion.enabled) {
        $hashName = [string]$HashAssertion.name
        $sequence.Add("hash|$hashName")
        $hashes[$hashName] = [ordered]@{
            description = ("Terminal harness hash assertion for key sequence: {0}" -f ($NormalizedKeys -join ","))
            start = [string]$HashAssertion.start
            size = [string]$HashAssertion.size
            expected_CRCs = @($HashAssertion.expected_crcs)
            timeout = [int]$HashAssertion.timeout_ms
        }
    }

    $template = [ordered]@{
        rom = "__ROM_PATH_SET_BY_SCRIPT__"
        transfer_files = @("__PROGRAM_PATH_SET_BY_SCRIPT__")
        target = [ordered]@{
            name = "CALCENG"
            isASM = $true
        }
        delay_after_key = 180
        delay_after_step = 350
        sequence = $sequence.ToArray()
        hashes = $hashes
    }

    $templatePath = [System.IO.Path]::Combine(
        [System.IO.Path]::GetTempPath(),
        ("calcemu-harness-template-{0}.json" -f [System.Guid]::NewGuid().ToString("N"))
    )

    $json = $template | ConvertTo-Json -Depth 12
    Set-Content -LiteralPath $templatePath -Value $json -Encoding UTF8
    return $templatePath
}

function Invoke-CalcEmuAutotestViaExistingWrapper {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WrapperPath,

        [Parameter(Mandatory = $true)]
        [string]$RomPath,

        [Parameter(Mandatory = $true)]
        [string]$AutotesterPath,

        [Parameter(Mandatory = $true)]
        [string]$ProgramPath,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ModuleProgramPaths = @(),

        [Parameter(Mandatory = $true)]
        [string[]]$NormalizedKeys,

        [Parameter(Mandatory = $true)]
        [object]$HashAssertion
    )

    $templatePath = New-CalcEmuAutotestTemplateFromKeys -NormalizedKeys $NormalizedKeys -HashAssertion $HashAssertion
    $generatedConfigPath = [System.IO.Path]::Combine(
        [System.IO.Path]::GetTempPath(),
        ("calcemu-harness-generated-{0}.json" -f [System.Guid]::NewGuid().ToString("N"))
    )
    $moduleProgramPathFile = ""
    if ($ModuleProgramPaths.Count -gt 0) {
        $moduleProgramPathFile = [System.IO.Path]::Combine(
            [System.IO.Path]::GetTempPath(),
            ("calcemu-harness-modules-{0}.txt" -f [System.Guid]::NewGuid().ToString("N"))
        )
        Set-Content -LiteralPath $moduleProgramPathFile -Value @($ModuleProgramPaths) -Encoding UTF8
    }

    $powershellCommand = Get-Command -Name "powershell.exe" -ErrorAction SilentlyContinue
    if ($null -eq $powershellCommand) {
        throw "Unable to locate powershell.exe to run the existing wrapper script."
    }

    $powershellPath = $powershellCommand.Source
    $arguments = @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $WrapperPath,
        "-RomPath",
        $RomPath,
        "-AutotesterPath",
        $AutotesterPath,
        "-ProgramPath",
        $ProgramPath,
        "-TemplatePath",
        $templatePath,
        "-OutputConfigPath",
        $generatedConfigPath
    )

    if (-not [string]::IsNullOrWhiteSpace($moduleProgramPathFile)) {
        $arguments += @(
            "-ModuleProgramPathsFile",
            $moduleProgramPathFile
        )
    }

    $displayParts = @($powershellPath)
    foreach ($argument in $arguments) {
        if ($argument -match "\s") {
            $displayParts += ('"{0}"' -f $argument.Replace('"', '\"'))
        }
        else {
            $displayParts += $argument
        }
    }

    $captured = @()
    $exitCode = 1
    $invocationError = ""

    try {
        $captured = & $powershellPath @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = 1
    }

    $outputLines = New-Object System.Collections.Generic.List[string]
    foreach ($entry in $captured) {
        if ($null -ne $entry) {
            $outputLines.Add([string]$entry)
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($invocationError)) {
        $outputLines.Add("Harness wrapper invocation error: $invocationError")
    }

    $transferFiles = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $generatedConfigPath -PathType Leaf) {
        try {
            $generatedConfig = Get-Content -LiteralPath $generatedConfigPath -Raw | ConvertFrom-Json
            foreach ($transferFile in @($generatedConfig.transfer_files)) {
                if ($null -ne $transferFile) {
                    $transferFiles.Add([string]$transferFile)
                }
            }
        }
        catch {
            $outputLines.Add("Harness wrapper config parse error: $($_.Exception.Message)")
        }
    }

    $hashObservation = Get-CalcEmuHashObservationFromOutput `
        -OutputLines $outputLines.ToArray() `
        -HashName ([string]$HashAssertion.name)

    return [ordered]@{
        attempted = $true
        wrapper_path = $WrapperPath
        template_path = $templatePath
        generated_config_path = $generatedConfigPath
        command = ($displayParts -join " ")
        exit_code = $exitCode
        output_lines = $outputLines.ToArray()
        transfer_files = $transferFiles.ToArray()
        transfer_file_count = $transferFiles.Count
        hash_assertion = [ordered]@{
            enabled = [bool]$HashAssertion.enabled
            name = [string]$HashAssertion.name
            start = [string]$HashAssertion.start
            size = [string]$HashAssertion.size
            timeout_ms = $HashAssertion.timeout_ms
            expected_crcs = @($HashAssertion.expected_crcs)
            observed_crcs = @($hashObservation.observed_crcs)
            last_observed_crc = [string]$hashObservation.last_observed_crc
            matched = $null
        }
    }
}

function Invoke-CalcEmuTerminalHarness {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$KeySequence,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$RomPath = $env:CEMU_ROM_PATH,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$AutotesterPath = $env:CEMU_AUTOTESTER_PATH,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ProgramPath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ModuleProgramPaths = @(),

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$UsableUserRamLimitBytes = 65536,

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$ModuleLinkedBinaryLimitBytes = 51200,

        [Parameter(Mandatory = $false)]
        [AllowEmptyString()]
        [string]$ArchiveRamDispatchModulePath = "",

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ArchiveRamModulePaths = @(),

        [Parameter(Mandatory = $false)]
        [ValidateRange(1024, 1048576)]
        [int]$ArchiveRamSwapRamCapacityBytes = 65536,

        [Parameter(Mandatory = $false)]
        [ValidateSet("none", "dispatch_load", "return_to_archive", "partial_load", "memory_pressure", "archive_full")]
        [string]$ArchiveRamFailurePhase = "none",

        [Parameter(Mandatory = $false)]
        [ValidateRange(1, 256)]
        [int]$ArchiveRamRapidSwapCycles = 1,

        [Parameter(Mandatory = $false)]
        [ValidateRange(0, 1048576)]
        [int]$ArchiveRamReservedRamBytes = 0,

        [Parameter(Mandatory = $false)]
        [switch]$ArchiveRamSimulatePartialLoad,

        [Parameter(Mandatory = $false)]
        [switch]$ForceMock,

        [Parameter(Mandatory = $false)]
        [switch]$NoMockFallback,

        [Parameter(Mandatory = $false)]
        [AllowEmptyCollection()]
        [string[]]$ExpectedVramCrcs = @(),

        [Parameter(Mandatory = $false)]
        [string]$CEmuHashName = "terminal_vram",

        [Parameter(Mandatory = $false)]
        [string]$CEmuHashStart = "vram_start",

        [Parameter(Mandatory = $false)]
        [string]$CEmuHashSize = "vram_16_size",

        [Parameter(Mandatory = $false)]
        [ValidateRange(1, 120000)]
        [int]$CEmuHashTimeoutMs = 8000
    )

    $normalizedExpectedCrcs = ConvertTo-CalcEmuExpectedCrcList -ExpectedVramCrcs $ExpectedVramCrcs
    $hashAssertionEnabled = ($normalizedExpectedCrcs.Count -gt 0)
    if ($hashAssertionEnabled) {
        if ([string]::IsNullOrWhiteSpace($CEmuHashName)) {
            throw "CEmuHashName cannot be empty when ExpectedVramCrcs is specified."
        }

        if ([string]::IsNullOrWhiteSpace($CEmuHashStart)) {
            throw "CEmuHashStart cannot be empty when ExpectedVramCrcs is specified."
        }

        if ([string]::IsNullOrWhiteSpace($CEmuHashSize)) {
            throw "CEmuHashSize cannot be empty when ExpectedVramCrcs is specified."
        }

        if ($CEmuHashName -notmatch "^[A-Za-z0-9_]+$") {
            throw "CEmuHashName '$CEmuHashName' is invalid. Only letters, numbers, and underscores are supported."
        }
    }

    $hashAssertion = [ordered]@{
        enabled = $hashAssertionEnabled
        name = ""
        start = ""
        size = ""
        timeout_ms = $null
        expected_crcs = @()
        observed_crcs = @()
        last_observed_crc = ""
        matched = $null
    }

    if ($hashAssertionEnabled) {
        $hashAssertion.name = $CEmuHashName
        $hashAssertion.start = $CEmuHashStart
        $hashAssertion.size = $CEmuHashSize
        $hashAssertion.timeout_ms = $CEmuHashTimeoutMs
        $hashAssertion.expected_crcs = $normalizedExpectedCrcs
    }

    $normalizedKeys = ConvertTo-CalcEmuKeySequence -KeySequence $KeySequence
    $screenState = Get-CalcEmuMockScreenState -NormalizedKeys $normalizedKeys

    $mode = "mock"
    $success = $true
    $errors = New-Object System.Collections.Generic.List[string]
    $fallbackReasons = New-Object System.Collections.Generic.List[string]
    $fallbackUsed = $true

    $autotest = [ordered]@{
        attempted = $false
        wrapper_path = ""
        template_path = ""
        generated_config_path = ""
        command = ""
        exit_code = $null
        output_lines = @()
        transfer_files = @()
        transfer_file_count = 0
        hash_assertion = [ordered]@{
            enabled = [bool]$hashAssertion.enabled
            name = [string]$hashAssertion.name
            start = [string]$hashAssertion.start
            size = [string]$hashAssertion.size
            timeout_ms = $hashAssertion.timeout_ms
            expected_crcs = @($hashAssertion.expected_crcs)
            observed_crcs = @($hashAssertion.observed_crcs)
            last_observed_crc = [string]$hashAssertion.last_observed_crc
            matched = $null
        }
    }

    $resolvedProgramPathForConstraintValidation = ""
    if (-not [string]::IsNullOrWhiteSpace($ProgramPath)) {
        $resolvedProgramPathForConstraintValidation = $ProgramPath
    }

    $hardwareConstraints = $null
    $hardwareConstraintErrorsApplied = $false

    if ($ForceMock.IsPresent) {
        $fallbackReasons.Add("ForceMock was specified.")

        $hardwareConstraints = Get-CalcEmuHardwareConstraintValidation `
            -ProgramPath $resolvedProgramPathForConstraintValidation `
            -ModuleProgramPaths $ModuleProgramPaths `
            -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
            -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes

        if ($hardwareConstraints.validated -and -not $hardwareConstraints.pass) {
            $success = $false
            foreach ($constraintError in $hardwareConstraints.errors) {
                $errors.Add([string]$constraintError)
            }
            $hardwareConstraintErrorsApplied = $true
        }
    }
    else {
        $readiness = Get-CalcEmuAutotestReadiness -RomPath $RomPath -AutotesterPath $AutotesterPath -ProgramPath $ProgramPath

        if ($readiness.can_run) {
            $fallbackUsed = $false
            if ([string]::IsNullOrWhiteSpace($resolvedProgramPathForConstraintValidation)) {
                $resolvedProgramPathForConstraintValidation = [string]$readiness.program_path
            }

            $hardwareConstraints = Get-CalcEmuHardwareConstraintValidation `
                -ProgramPath $resolvedProgramPathForConstraintValidation `
                -ModuleProgramPaths $ModuleProgramPaths `
                -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
                -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes

            if ($hardwareConstraints.validated -and -not $hardwareConstraints.pass) {
                $success = $false
                $mode = "error"
                $fallbackUsed = $false
                foreach ($constraintError in $hardwareConstraints.errors) {
                    $errors.Add([string]$constraintError)
                }
                $hardwareConstraintErrorsApplied = $true
            }
            else {
                $mode = "cemu"
                $autotest = Invoke-CalcEmuAutotestViaExistingWrapper `
                    -WrapperPath $readiness.wrapper_path `
                    -RomPath $readiness.rom_path `
                    -AutotesterPath $readiness.autotester_path `
                    -ProgramPath $readiness.program_path `
                    -ModuleProgramPaths $ModuleProgramPaths `
                    -NormalizedKeys $normalizedKeys `
                    -HashAssertion $hashAssertion

                if ($autotest.exit_code -ne 0) {
                    $success = $false
                    $errors.Add("CEmu autotest exited with code $($autotest.exit_code).")
                }
            }
        }
        else {
            foreach ($reason in $readiness.reasons) {
                $fallbackReasons.Add($reason)
            }

            if ($NoMockFallback.IsPresent) {
                $success = $false
                $mode = "error"
                $fallbackUsed = $false
                foreach ($reason in $readiness.reasons) {
                    $errors.Add($reason)
                }
            }
        }
    }

    if ($null -eq $hardwareConstraints) {
        $hardwareConstraints = Get-CalcEmuHardwareConstraintValidation `
            -ProgramPath $resolvedProgramPathForConstraintValidation `
            -ModuleProgramPaths $ModuleProgramPaths `
            -UsableUserRamLimitBytes $UsableUserRamLimitBytes `
            -ModuleLinkedBinaryLimitBytes $ModuleLinkedBinaryLimitBytes
    }

    if ($hardwareConstraints.validated -and -not $hardwareConstraints.pass -and -not $hardwareConstraintErrorsApplied) {
        $success = $false
        foreach ($constraintError in $hardwareConstraints.errors) {
            $errors.Add([string]$constraintError)
        }
        $hardwareConstraintErrorsApplied = $true
    }

    $archiveRamModulePathsForSimulation = @($ArchiveRamModulePaths)
    if ($archiveRamModulePathsForSimulation.Count -eq 0 -and $ModuleProgramPaths.Count -gt 0) {
        $archiveRamModulePathsForSimulation = @($ModuleProgramPaths)
    }

    $archiveRamSwap = Get-CalcEmuArchiveRamSwapSimulation `
        -DispatchModulePath $ArchiveRamDispatchModulePath `
        -ModulePaths $archiveRamModulePathsForSimulation `
        -SwapRamCapacityBytes $ArchiveRamSwapRamCapacityBytes `
        -FailurePhase $ArchiveRamFailurePhase `
        -RapidSwapCycles $ArchiveRamRapidSwapCycles `
        -ReservedRamBytes $ArchiveRamReservedRamBytes `
        -SimulatePartialLoad:$ArchiveRamSimulatePartialLoad

    if ($archiveRamSwap.simulated -and -not $archiveRamSwap.pass) {
        $success = $false
        foreach ($swapError in @($archiveRamSwap.errors)) {
            $errors.Add([string]$swapError)
        }
    }

    if ($hashAssertionEnabled -and $mode -ne "cemu") {
        $success = $false
        $errors.Add("CEmu hash assertion was requested, but the harness did not run in cemu mode.")
    }

    if ($autotest.hash_assertion.enabled -and $mode -eq "cemu" -and $null -ne $autotest.exit_code) {
        $autotest.hash_assertion.matched = ($autotest.exit_code -eq 0)
        if ($autotest.exit_code -ne 0 -and $autotest.hash_assertion.observed_crcs.Count -eq 0) {
            $errors.Add("CEmu hash verification failed and no observed CRC could be parsed from autotester output. Re-run with autotester debug output enabled to collect a baseline CRC.")
        }
    }

    if (
        -not $NoMockFallback.IsPresent -and
        $mode -eq "mock" -and
        -not $hashAssertionEnabled -and
        (
            -not $hardwareConstraints.validated -or
            $hardwareConstraints.pass
        ) -and
        (
            -not $archiveRamSwap.simulated -or
            $archiveRamSwap.pass
        )
    ) {
        $success = $true
    }

    return [ordered]@{
        schema_version = 1
        mode = $mode
        success = $success
        fallback = [ordered]@{
            used = $fallbackUsed
            reasons = $fallbackReasons.ToArray()
        }
        input = [ordered]@{
            keys_raw = $KeySequence
            keys_normalized = $normalizedKeys
        }
        screen = $screenState
        hardware_constraints = $hardwareConstraints
        archive_ram_swap = $archiveRamSwap
        autotest = $autotest
        errors = $errors.ToArray()
    }
}

function ConvertTo-CalcEmuTextScalar {
    param(
        [Parameter(Mandatory = $false)]
        [AllowNull()]
        [object]$Value
    )

    if ($null -eq $Value) {
        return "<none>"
    }

    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) {
        return "<none>"
    }

    $text = $text.Replace("`r", "")
    $text = $text.Replace("`n", "\n")
    return $text
}

function ConvertTo-CalcEmuTextBoolean {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Value
    )

    if ($Value) {
        return "true"
    }

    return "false"
}

function Format-CalcEmuHarnessTextOutput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $fallbackReasons = "<none>"
    if ($Result.fallback.reasons.Count -gt 0) {
        $fallbackReasons = ($Result.fallback.reasons -join " | ")
    }

    $screenIgnored = "<none>"
    if ($Result.screen.ignored_keys.Count -gt 0) {
        $screenIgnored = ($Result.screen.ignored_keys -join ",")
    }

    $autotestExitCode = "<none>"
    if ($null -ne $Result.autotest.exit_code) {
        $autotestExitCode = [string]$Result.autotest.exit_code
    }

    $autotestOutput = "<none>"
    if ($Result.autotest.output_lines.Count -gt 0) {
        $autotestOutput = ($Result.autotest.output_lines -join " | ")
    }

    $autotestHashEnabled = ConvertTo-CalcEmuTextBoolean -Value ([bool]$Result.autotest.hash_assertion.enabled)
    $autotestHashExpectedCrcs = "<none>"
    if ($Result.autotest.hash_assertion.expected_crcs.Count -gt 0) {
        $autotestHashExpectedCrcs = ($Result.autotest.hash_assertion.expected_crcs -join ",")
    }
    $autotestHashObservedCrcs = "<none>"
    if ($Result.autotest.hash_assertion.observed_crcs.Count -gt 0) {
        $autotestHashObservedCrcs = ($Result.autotest.hash_assertion.observed_crcs -join ",")
    }
    $autotestHashMatched = "<none>"
    if ($null -ne $Result.autotest.hash_assertion.matched) {
        $autotestHashMatched = ConvertTo-CalcEmuTextBoolean -Value ([bool]$Result.autotest.hash_assertion.matched)
    }

    $errors = "<none>"
    if ($Result.errors.Count -gt 0) {
        $errors = ($Result.errors -join " | ")
    }

    $hardwareConstraints = $Result.hardware_constraints
    if ($null -eq $hardwareConstraints) {
        $hardwareConstraints = [ordered]@{
            validated = $false
            pass = $true
            memory_insufficient = $false
            module_size_exceeded = $false
            usable_user_ram_limit_bytes = 0
            module_linked_binary_limit_bytes = 0
            program = [ordered]@{
                checked = $false
                size_bytes = $null
            }
            modules = [ordered]@{
                checked_count = 0
                over_limit_count = 0
            }
            errors = @()
        }
    }

    $hardwareConstraintErrors = "<none>"
    if ($hardwareConstraints.errors.Count -gt 0) {
        $hardwareConstraintErrors = ($hardwareConstraints.errors -join " | ")
    }

    $archiveRamSwap = $Result.archive_ram_swap
    if ($null -eq $archiveRamSwap) {
        $archiveRamSwap = [ordered]@{
            simulated = $false
            pass = $true
            failure_phase = "none"
            ram_capacity_bytes = 0
            dispatch = [ordered]@{
                requested = $false
                module_path = ""
                resolved_module_path = ""
                module_size_bytes = $null
                loaded_to_ram = $false
                error = ""
            }
            return_to_archive = [ordered]@{
                attempted = $false
                returned_to_archive = $false
                error = ""
            }
            state = [ordered]@{
                module_in_archive = $false
                module_in_ram = $false
            }
            modules = [ordered]@{
                requested_count = 0
                rapid_swap_cycles = 1
                reserved_ram_bytes = 0
                effective_ram_capacity_bytes = 0
                dispatch_events = 0
                return_events = 0
                corruption_detected = $false
                partial_load_detected = $false
                items = @()
            }
            diagnostics = @()
            errors = @()
        }
    }

    $archiveRamModules = $archiveRamSwap.modules
    if ($null -eq $archiveRamModules) {
        $archiveRamModules = [ordered]@{
            requested_count = 0
            rapid_swap_cycles = 1
            reserved_ram_bytes = 0
            effective_ram_capacity_bytes = 0
            dispatch_events = 0
            return_events = 0
            corruption_detected = $false
            partial_load_detected = $false
            items = @()
        }
    }

    $archiveRamDiagnostics = "<none>"
    if ($archiveRamSwap.diagnostics.Count -gt 0) {
        $archiveRamDiagnostics = ($archiveRamSwap.diagnostics -join " | ")
    }

    $archiveRamErrors = "<none>"
    if ($archiveRamSwap.errors.Count -gt 0) {
        $archiveRamErrors = ($archiveRamSwap.errors -join " | ")
    }

    $lines = @(
        ("schema_version={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.schema_version)),
        ("mode={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.mode)),
        ("success={0}" -f (ConvertTo-CalcEmuTextBoolean -Value $Result.success)),
        ("fallback_used={0}" -f (ConvertTo-CalcEmuTextBoolean -Value $Result.fallback.used)),
        ("fallback_reasons={0}" -f (ConvertTo-CalcEmuTextScalar -Value $fallbackReasons)),
        ("keys_normalized={0}" -f (ConvertTo-CalcEmuTextScalar -Value ($Result.input.keys_normalized -join ","))),
        ("screen_expression={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.screen.expression)),
        ("screen_result={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.screen.result)),
        ("screen_status={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.screen.status)),
        ("screen_error={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.screen.error)),
        ("screen_ignored_keys={0}" -f (ConvertTo-CalcEmuTextScalar -Value $screenIgnored)),
        ("hardware_constraints_validated={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$hardwareConstraints.validated))),
        ("hardware_constraints_pass={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$hardwareConstraints.pass))),
        ("hardware_constraints_memory_insufficient={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$hardwareConstraints.memory_insufficient))),
        ("hardware_constraints_module_size_exceeded={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$hardwareConstraints.module_size_exceeded))),
        ("hardware_constraints_program_checked={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$hardwareConstraints.program.checked))),
        ("hardware_constraints_program_size_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraints.program.size_bytes)),
        ("hardware_constraints_program_limit_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraints.usable_user_ram_limit_bytes)),
        ("hardware_constraints_module_checked_count={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraints.modules.checked_count)),
        ("hardware_constraints_module_over_limit_count={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraints.modules.over_limit_count)),
        ("hardware_constraints_module_limit_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraints.module_linked_binary_limit_bytes)),
        ("hardware_constraints_errors={0}" -f (ConvertTo-CalcEmuTextScalar -Value $hardwareConstraintErrors)),
        ("archive_ram_swap_simulated={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.simulated))),
        ("archive_ram_swap_pass={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.pass))),
        ("archive_ram_swap_failure_phase={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamSwap.failure_phase)),
        ("archive_ram_swap_ram_capacity_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamSwap.ram_capacity_bytes)),
        ("archive_ram_swap_dispatch_requested={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.dispatch.requested))),
        ("archive_ram_swap_dispatch_module_path={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamSwap.dispatch.module_path)),
        ("archive_ram_swap_dispatch_module_size_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamSwap.dispatch.module_size_bytes)),
        ("archive_ram_swap_dispatch_loaded_to_ram={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.dispatch.loaded_to_ram))),
        ("archive_ram_swap_return_attempted={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.return_to_archive.attempted))),
        ("archive_ram_swap_return_completed={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.return_to_archive.returned_to_archive))),
        ("archive_ram_swap_state_module_in_archive={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.state.module_in_archive))),
        ("archive_ram_swap_state_module_in_ram={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamSwap.state.module_in_ram))),
        ("archive_ram_swap_module_count={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.requested_count)),
        ("archive_ram_swap_rapid_swap_cycles={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.rapid_swap_cycles)),
        ("archive_ram_swap_reserved_ram_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.reserved_ram_bytes)),
        ("archive_ram_swap_effective_ram_capacity_bytes={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.effective_ram_capacity_bytes)),
        ("archive_ram_swap_dispatch_events={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.dispatch_events)),
        ("archive_ram_swap_return_events={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamModules.return_events)),
        ("archive_ram_swap_corruption_detected={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamModules.corruption_detected))),
        ("archive_ram_swap_partial_load_detected={0}" -f (ConvertTo-CalcEmuTextBoolean -Value ([bool]$archiveRamModules.partial_load_detected))),
        ("archive_ram_swap_diagnostics={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamDiagnostics)),
        ("archive_ram_swap_errors={0}" -f (ConvertTo-CalcEmuTextScalar -Value $archiveRamErrors)),
        ("autotest_attempted={0}" -f (ConvertTo-CalcEmuTextBoolean -Value $Result.autotest.attempted)),
        ("autotest_exit_code={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestExitCode)),
        ("autotest_generated_config_path={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.generated_config_path)),
        ("autotest_hash_enabled={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestHashEnabled)),
        ("autotest_hash_name={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.hash_assertion.name)),
        ("autotest_hash_start={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.hash_assertion.start)),
        ("autotest_hash_size={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.hash_assertion.size)),
        ("autotest_hash_timeout_ms={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.hash_assertion.timeout_ms)),
        ("autotest_hash_expected_crcs={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestHashExpectedCrcs)),
        ("autotest_hash_observed_crcs={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestHashObservedCrcs)),
        ("autotest_hash_last_observed_crc={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.autotest.hash_assertion.last_observed_crc)),
        ("autotest_hash_matched={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestHashMatched)),
        ("autotest_output_lines={0}" -f (ConvertTo-CalcEmuTextScalar -Value $autotestOutput)),
        ("errors={0}" -f (ConvertTo-CalcEmuTextScalar -Value $errors)),
        ("screen_text={0}" -f (ConvertTo-CalcEmuTextScalar -Value $Result.screen.text))
    )

    return ($lines -join "`n")
}

function ConvertTo-CalcEmuHarnessJson {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    return ($Result | ConvertTo-Json -Depth 12)
}

Export-ModuleMember -Function `
    ConvertTo-CalcEmuHarnessJson, `
    ConvertTo-CalcEmuKeySequence, `
    Format-CalcEmuHarnessTextOutput, `
    Get-CalcEmuMockScreenState, `
    Invoke-CalcEmuTerminalHarness
