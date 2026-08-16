param(
    [int]$Jobs = 4,
    [string]$DevkitPro = ""
)

$ErrorActionPreference = "Stop"

$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($DevkitPro)) { $candidates += $DevkitPro }
if (-not [string]::IsNullOrWhiteSpace($env:DEVKITPRO) -and
    $env:DEVKITPRO -notmatch '^/opt/devkitpro/?$') {
    $candidates += $env:DEVKITPRO
}
$candidates += "C:\devkitPro"

$devkitRoot = $null
foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate "libnx\switch_rules")) {
        $devkitRoot = $candidate
        break
    }
}
if ($null -eq $devkitRoot) {
    throw "Could not find devkitPro/libnx under: $($candidates -join ', ')"
}

$msysBin = Join-Path $devkitRoot "msys2\usr\bin"
$cygpath = Join-Path $msysBin "cygpath.exe"
$make = Join-Path $msysBin "make.exe"
$gxx = Join-Path $msysBin "g++.exe"
foreach ($tool in @($cygpath, $make, $gxx)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" }
}

function Convert-ToMsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $converted = & $cygpath -u $Path
    if ($LASTEXITCODE -ne 0) { throw "cygpath failed for $Path" }
    return $converted.Trim()
}

$phase2Windows = Join-Path $PSScriptRoot "phase2"
$phase2 = Convert-ToMsysPath $phase2Windows
$hostBuildWindows = Join-Path $phase2Windows "build_host"
$hostBuild = Convert-ToMsysPath $hostBuildWindows

New-Item -ItemType Directory -Force -Path $hostBuildWindows | Out-Null

$env:Path = "$devkitRoot\devkitA64\bin;$devkitRoot\tools\bin;$msysBin;$env:Path"
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"

Write-Host "Running host-side ABI and mailbox tests..."
$hostCompilerArgs = @(
    "-std=c++20",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I$phase2/common/include",
    "$phase2/common/source/service_core.cpp",
    "$phase2/tests/service_core_tests.cpp",
    "-o",
    "$hostBuild/service-core-tests.exe"
)
& $gxx @hostCompilerArgs
if ($LASTEXITCODE -ne 0) { throw "Host test build failed with exit code $LASTEXITCODE" }

& (Join-Path $hostBuildWindows "service-core-tests.exe")
if ($LASTEXITCODE -ne 0) { throw "Host tests failed with exit code $LASTEXITCODE" }

Write-Host "Building Switify background-service ExeFS..."
& $make "-C" $phase2 "-j$Jobs" "dist"
if ($LASTEXITCODE -ne 0) { throw "Phase 2 Switch build failed with exit code $LASTEXITCODE" }

$healthZip = Join-Path $phase2Windows "Switify-Phase2-health-0.1.6.zip"
Compress-Archive -Path (Join-Path $phase2Windows "dist\*") `
    -DestinationPath $healthZip -Force

Write-Host ""
Write-Host "Phase 2 health build complete."
Write-Host "ExeFS: $(Join-Path $phase2Windows 'sysmodule\switify-sysmodule.nsp')"
Write-Host "Overlay: $(Join-Path $phase2Windows 'overlay\switify-health-overlay.ovl')"
Write-Host "Developer-only probe: $(Join-Path $phase2Windows 'probe\switify-service-probe.nro')"
Write-Host "SD tree: $(Join-Path $phase2Windows 'dist')"
Write-Host "Install zip: $healthZip"
