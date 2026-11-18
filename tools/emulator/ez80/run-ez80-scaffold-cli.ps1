param(
    [Parameter(Mandatory = $false)]
    [ValidateSet("probe-rom")]
    [string]$Command = "probe-rom",

    [Parameter(Mandatory = $false)]
    [string]$RomPath = "rom/Rom.rom",

    [Parameter(Mandatory = $false)]
    [ValidateSet("json", "text")]
    [string]$Format = "json",

    [Parameter(Mandatory = $false)]
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Ez80RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
}

function ConvertTo-Ez80CanonicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathInput
    )

    if ([string]::IsNullOrWhiteSpace($PathInput)) {
        return ""
    }

    if ([System.IO.Path]::IsPathRooted($PathInput)) {
        return [System.IO.Path]::GetFullPath($PathInput)
    }

    $repoRoot = Resolve-Ez80RepoRoot
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathInput))
}

function Get-Ez80Crc32Hex {
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

function ConvertTo-Ez80AsciiPreview {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes
    )

    $builder = New-Object System.Text.StringBuilder
    foreach ($value in $Bytes) {
        if ($value -ge 32 -and $value -le 126) {
            [void]$builder.Append([char]$value)
        }
        else {
            [void]$builder.Append(".")
        }
    }

    return $builder.ToString()
}

function ConvertTo-Ez80FirstHex {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes,

        [Parameter(Mandatory = $true)]
        [int]$TakeCount
    )

    if ($Bytes.Length -eq 0) {
        return ""
    }

    $maxCount = [Math]::Min($Bytes.Length, $TakeCount)
    $chunks = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $maxCount; $i++) {
        $chunks.Add(("{0:X2}" -f [int]$Bytes[$i]))
    }

    return ($chunks.ToArray() -join " ")
}

function Invoke-Ez80ProbeRom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RomPathInput
    )

    $romPathResolved = ConvertTo-Ez80CanonicalPath -PathInput $RomPathInput
    $nowUtc = (Get-Date).ToUniversalTime().ToString("o")
    $expectedSize = 4MB

    if ([string]::IsNullOrWhiteSpace($romPathResolved)) {
        return [ordered]@{
            schema_version = 1
            command = "probe-rom"
            generated_at_utc = $nowUtc
            success = $false
            status = "error"
            error = "RomPath cannot be empty."
            rom = [ordered]@{
                input_path = $RomPathInput
                resolved_path = ""
                exists = $false
                file_size_bytes = 0
                expected_size_bytes = $expectedSize
                size_matches_expected = $false
                crc32 = ""
                crc32_algorithm = "CRC32-IEEE-802.3"
                header_probe = [ordered]@{
                    bytes_sampled = 0
                    first_16_hex = ""
                    ascii_preview = ""
                    has_nonzero_prefix = $false
                    contains_ti_marker = $false
                }
            }
            scaffold = [ordered]@{
                implementation_stage = "pass0_stub"
                boots_tios = $false
                executes_instructions = $false
                notes = "ROM probe scaffold only. CPU emulation is not wired yet."
            }
        }
    }

    if (-not (Test-Path -LiteralPath $romPathResolved -PathType Leaf)) {
        return [ordered]@{
            schema_version = 1
            command = "probe-rom"
            generated_at_utc = $nowUtc
            success = $false
            status = "error"
            error = "RomPath '$romPathResolved' does not exist."
            rom = [ordered]@{
                input_path = $RomPathInput
                resolved_path = $romPathResolved
                exists = $false
                file_size_bytes = 0
                expected_size_bytes = $expectedSize
                size_matches_expected = $false
                crc32 = ""
                crc32_algorithm = "CRC32-IEEE-802.3"
                header_probe = [ordered]@{
                    bytes_sampled = 0
                    first_16_hex = ""
                    ascii_preview = ""
                    has_nonzero_prefix = $false
                    contains_ti_marker = $false
                }
            }
            scaffold = [ordered]@{
                implementation_stage = "pass0_stub"
                boots_tios = $false
                executes_instructions = $false
                notes = "ROM probe scaffold only. CPU emulation is not wired yet."
            }
        }
    }

    $bytes = [System.IO.File]::ReadAllBytes($romPathResolved)
    $fileSize = [int64]$bytes.Length
    $crc32 = Get-Ez80Crc32Hex -Data $bytes
    $headerBytesToInspect = [Math]::Min(32, $bytes.Length)
    $headerBytes = New-Object byte[] $headerBytesToInspect
    if ($headerBytesToInspect -gt 0) {
        [Array]::Copy($bytes, 0, $headerBytes, 0, $headerBytesToInspect)
    }

    $asciiPreview = ConvertTo-Ez80AsciiPreview -Bytes $headerBytes
    $hasNonZeroPrefix = $false
    foreach ($byteValue in $headerBytes) {
        if ($byteValue -ne 0) {
            $hasNonZeroPrefix = $true
            break
        }
    }

    $containsTiMarker = $false
    if (-not [string]::IsNullOrWhiteSpace($asciiPreview)) {
        $containsTiMarker = ($asciiPreview.IndexOf("TI", [System.StringComparison]::OrdinalIgnoreCase) -ge 0)
    }

    return [ordered]@{
        schema_version = 1
        command = "probe-rom"
        generated_at_utc = $nowUtc
        success = $true
        status = "ok"
        error = ""
        rom = [ordered]@{
            input_path = $RomPathInput
            resolved_path = $romPathResolved
            exists = $true
            file_size_bytes = $fileSize
            expected_size_bytes = $expectedSize
            size_matches_expected = ($fileSize -eq $expectedSize)
            crc32 = $crc32
            crc32_algorithm = "CRC32-IEEE-802.3"
            header_probe = [ordered]@{
                bytes_sampled = $headerBytesToInspect
                first_16_hex = (ConvertTo-Ez80FirstHex -Bytes $headerBytes -TakeCount 16)
                ascii_preview = $asciiPreview
                has_nonzero_prefix = $hasNonZeroPrefix
                contains_ti_marker = $containsTiMarker
            }
        }
        scaffold = [ordered]@{
            implementation_stage = "pass0_stub"
            boots_tios = $false
            executes_instructions = $false
            notes = "ROM probe scaffold only. CPU emulation is not wired yet."
        }
    }
}

function ConvertTo-Ez80ProbeText {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $lines = @(
        ("schema_version={0}" -f $Result.schema_version),
        ("command={0}" -f $Result.command),
        ("generated_at_utc={0}" -f $Result.generated_at_utc),
        ("status={0}" -f $Result.status),
        ("success={0}" -f ([string]$Result.success).ToLowerInvariant()),
        ("error={0}" -f $Result.error),
        ("rom_input_path={0}" -f $Result.rom.input_path),
        ("rom_resolved_path={0}" -f $Result.rom.resolved_path),
        ("rom_exists={0}" -f ([string]$Result.rom.exists).ToLowerInvariant()),
        ("rom_size_bytes={0}" -f $Result.rom.file_size_bytes),
        ("rom_expected_size_bytes={0}" -f $Result.rom.expected_size_bytes),
        ("rom_size_matches_expected={0}" -f ([string]$Result.rom.size_matches_expected).ToLowerInvariant()),
        ("rom_crc32={0}" -f $Result.rom.crc32),
        ("rom_crc32_algorithm={0}" -f $Result.rom.crc32_algorithm),
        ("rom_header_bytes_sampled={0}" -f $Result.rom.header_probe.bytes_sampled),
        ("rom_header_first_16_hex={0}" -f $Result.rom.header_probe.first_16_hex),
        ("rom_header_ascii_preview={0}" -f $Result.rom.header_probe.ascii_preview),
        ("rom_header_has_nonzero_prefix={0}" -f ([string]$Result.rom.header_probe.has_nonzero_prefix).ToLowerInvariant()),
        ("rom_header_contains_ti_marker={0}" -f ([string]$Result.rom.header_probe.contains_ti_marker).ToLowerInvariant()),
        ("scaffold_implementation_stage={0}" -f $Result.scaffold.implementation_stage),
        ("scaffold_boots_tios={0}" -f ([string]$Result.scaffold.boots_tios).ToLowerInvariant()),
        ("scaffold_executes_instructions={0}" -f ([string]$Result.scaffold.executes_instructions).ToLowerInvariant()),
        ("scaffold_notes={0}" -f $Result.scaffold.notes)
    )

    return ($lines -join "`n")
}

try {
    $result = $null
    switch ($Command) {
        "probe-rom" {
            $result = Invoke-Ez80ProbeRom -RomPathInput $RomPath
        }
        default {
            throw "Unsupported command '$Command'."
        }
    }

    $rendered = ""
    if ($Format -eq "json") {
        $rendered = ($result | ConvertTo-Json -Depth 8)
    }
    else {
        $rendered = ConvertTo-Ez80ProbeText -Result $result
    }

    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        Set-Content -LiteralPath $OutputPath -Value $rendered -Encoding UTF8
    }

    Write-Output $rendered
    if ($result.success) {
        exit 0
    }

    exit 1
}
catch {
    $errorPayload = [ordered]@{
        schema_version = 1
        command = $Command
        generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        success = $false
        status = "error"
        error = $_.Exception.Message
    }

    $renderedError = ""
    if ($Format -eq "json") {
        $renderedError = ($errorPayload | ConvertTo-Json -Depth 4)
    }
    else {
        $renderedError = @(
            ("schema_version={0}" -f $errorPayload.schema_version),
            ("command={0}" -f $errorPayload.command),
            ("generated_at_utc={0}" -f $errorPayload.generated_at_utc),
            ("status={0}" -f $errorPayload.status),
            ("success=false"),
            ("error={0}" -f $errorPayload.error)
        ) -join "`n"
    }

    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        Set-Content -LiteralPath $OutputPath -Value $renderedError -Encoding UTF8
    }

    Write-Output $renderedError
    exit 2
}
