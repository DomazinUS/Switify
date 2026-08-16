param(
    [Parameter(Mandatory = $true)]
    [string]$CrashReport,
    [string]$Elf = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Elf)) {
    $Elf = Join-Path $PSScriptRoot "build_app\Switify.elf"
}
$CrashReport = (Resolve-Path -LiteralPath $CrashReport).Path
$Elf = (Resolve-Path -LiteralPath $Elf).Path

$toolRoot = "C:\devkitPro\devkitA64\bin"
$addr2line = Join-Path $toolRoot "aarch64-none-elf-addr2line.exe"
$readelf = Join-Path $toolRoot "aarch64-none-elf-readelf.exe"
if (-not (Test-Path -LiteralPath $addr2line)) {
    throw "addr2line was not found at $addr2line."
}

$report = Get-Content -LiteralPath $CrashReport -Raw
$primaryReport = ($report -split '(?m)^Thread Report:', 2)[0]
$moduleMatch = [regex]::Match($report, '(?m)^\s*Module Id:\s*([0-9A-Fa-f]+)')
$elfNotes = if (Test-Path -LiteralPath $readelf) {
    (& $readelf -n $Elf 2>&1) -join "`n"
} else { "" }
$buildMatch = [regex]::Match($elfNotes, 'Build ID:\s*([0-9A-Fa-f]+)')

if ($moduleMatch.Success -and $buildMatch.Success) {
    $crashId = $moduleMatch.Groups[1].Value.Substring(0, 40).ToLowerInvariant()
    $elfId = $buildMatch.Groups[1].Value.ToLowerInvariant()
    if ($crashId -ne $elfId) {
        Write-Warning "ELF mismatch: crash=$crashId, ELF=$elfId. Symbols below may be wrong."
    } else {
        Write-Host "ELF build ID matches the crash: $elfId"
    }
}

$addresses = [System.Collections.Generic.List[string]]::new()
$labels = [System.Collections.Generic.List[string]]::new()

$pcMatch = [regex]::Match($primaryReport, '(?m)^\s*PC:.*\(Switify \+ 0x([0-9A-Fa-f]+)\)')
if ($pcMatch.Success) {
    $addresses.Add("0x$($pcMatch.Groups[1].Value)")
    $labels.Add("PC")
}

$returnMatches = [regex]::Matches(
    $primaryReport,
    '(?m)^\s*ReturnAddress\[(\d+)\]:.*\(Switify \+ 0x([0-9A-Fa-f]+)\)')
foreach ($match in $returnMatches) {
    $value = [Convert]::ToUInt64($match.Groups[2].Value, 16)
    if ($value -ge 4) { $value -= 4 }
    $addresses.Add(('0x{0:x}' -f $value))
    $labels.Add("ReturnAddress[$($match.Groups[1].Value)] - 4")
}

if ($addresses.Count -eq 0) {
    throw "No Switify PC or return addresses were found in $CrashReport."
}

Write-Host ""
Write-Host "Crash: $CrashReport"
Write-Host "ELF:   $Elf"
Write-Host ""

for ($index = 0; $index -lt $addresses.Count; $index++) {
    Write-Host "=== $($labels[$index]) @ $($addresses[$index]) ==="
    & $addr2line -f -C -i -p -e $Elf $addresses[$index]
}
