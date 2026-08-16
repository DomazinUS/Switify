param(
    [int]$Jobs = 4,
    [string]$DevkitPro = ""
)

$ErrorActionPreference = "Stop"

# --- Resolve the devkitPro root (Windows path). The installer sets DEVKITPRO
# to the MSYS2-style /opt/devkitpro, which maps to C:\devkitPro on disk.
$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($DevkitPro)) { $candidates += $DevkitPro }
if (-not [string]::IsNullOrWhiteSpace($env:DEVKITPRO)) {
    if ($env:DEVKITPRO -match '^/opt/devkitpro/?$') {
        $candidates += "C:\devkitPro"
    } else {
        $candidates += $env:DEVKITPRO
    }
}
$candidates += "C:\devkitPro"

$root = $null
foreach ($c in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $c "cmake\Switch.cmake")) {
        $root = $c
        break
    }
}
if ($null -eq $root) {
    throw "Could not find cmake\Switch.cmake under any of: $($candidates -join ', '). Install it in the devkitPro MSYS2 shell: pacman -S switch-cmake"
}
Write-Host "devkitPro root: $root"

# --- Prefer devkitPro's own MSYS2 cmake/ninja/cygpath. The cmake found on a
# normal Windows PATH here is MSYS-flavored and mistreats absolute Windows
# paths (C:\... becomes a relative path), so all paths handed to it must be
# converted with cygpath — same approach as twiNX's build script.
$msysBin = Join-Path $root "msys2\usr\bin"
$cygpath = Join-Path $msysBin "cygpath.exe"
$useMsys = Test-Path -LiteralPath $cygpath

function Convert-BuildPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ($useMsys) {
        $converted = & $cygpath -u $Path
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($converted)) {
            return $converted.Trim()
        }
    }
    return ($Path -replace '\\', '/')
}

$cmakeExe = "cmake"
$ninjaExe = "ninja"
if ($useMsys) {
    if (Test-Path -LiteralPath (Join-Path $msysBin "cmake.exe")) { $cmakeExe = Join-Path $msysBin "cmake.exe" }
    if (Test-Path -LiteralPath (Join-Path $msysBin "ninja.exe")) { $ninjaExe = Join-Path $msysBin "ninja.exe" }
}

$env:Path = "$root\devkitA64\bin;$root\tools\bin;$msysBin;$env:Path"

# Under MSYS2 tools, devkitPro lives at its canonical mount /opt/devkitpro
if ($useMsys) {
    $env:DEVKITPRO = "/opt/devkitpro"
    $env:DEVKITA64 = "/opt/devkitpro/devkitA64"
    $toolchain = "/opt/devkitpro/cmake/Switch.cmake"
} else {
    $env:DEVKITPRO = ($root -replace '\\', '/')
    $env:DEVKITA64 = "$env:DEVKITPRO/devkitA64"
    $toolchain = (Join-Path $root "cmake\Switch.cmake") -replace '\\', '/'
}

# Files delivered from the cloud session can carry timestamps a few hours in
# the future (UTC container vs local clock); ninja then loops with "manifest
# still dirty". Clamp any future mtime to now before building.
$now = Get-Date
Get-ChildItem -Path $PSScriptRoot -Recurse -File | Where-Object { $_.LastWriteTime -gt $now } | ForEach-Object { $_.LastWriteTime = $now }

# Runnable from anywhere: source and build paths are anchored to this script
$sourceDir = Convert-BuildPath (Join-Path $PSScriptRoot "app")
$buildDir = Convert-BuildPath (Join-Path $PSScriptRoot "build_app")

$cmakeArgs = @(
    "-S", $sourceDir,
    "-B", $buildDir,
    "-G", "Ninja",
    "-DPLATFORM_SWITCH=ON",
    "-DCMAKE_BUILD_TYPE=Release",
    # Vendored cspot/bell declare pre-3.5 CMake minimums; CMake 4.x refuses
    # those without this compatibility floor.
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.13"
)

& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE." }

& $ninjaExe -C $buildDir "-j$Jobs" Switify.nro
if ($LASTEXITCODE -ne 0) { throw "Nintendo Switch build failed with exit code $LASTEXITCODE." }

Write-Host ""
Write-Host "Output: $(Join-Path $PSScriptRoot 'build_app\Switify.nro')"
