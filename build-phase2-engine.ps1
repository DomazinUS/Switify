param(
    [int]$Jobs = 4,
    [string]$DevkitPro = "",
    [switch]$Lightweight
)

$ErrorActionPreference = "Stop"
$engineVersion = if ($Lightweight) { "0.5.3-lite1" } else { "0.5.3" }

$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($DevkitPro)) { $candidates += $DevkitPro }
if (-not [string]::IsNullOrWhiteSpace($env:DEVKITPRO) -and
    $env:DEVKITPRO -notmatch '^/opt/devkitpro/?$') {
    $candidates += $env:DEVKITPRO
}
$candidates += "C:\devkitPro"

$devkitRoot = $null
foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate "cmake\Switch.cmake")) {
        $devkitRoot = $candidate
        break
    }
}
if ($null -eq $devkitRoot) { throw "Could not find the devkitPro Switch toolchain" }

$msysBin = Join-Path $devkitRoot "msys2\usr\bin"
$cygpath = Join-Path $msysBin "cygpath.exe"
$cmake = Join-Path $msysBin "cmake.exe"
$ninja = Join-Path $msysBin "ninja.exe"
$make = Join-Path $msysBin "make.exe"

function Convert-ToMsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $converted = & $cygpath -u $Path
    if ($LASTEXITCODE -ne 0) { throw "cygpath failed for $Path" }
    return $converted.Trim()
}

$source = Convert-ToMsysPath (Join-Path $PSScriptRoot "phase2\sysmodule")
$buildName = if ($Lightweight) {
    "build_engine_lightweight"
} else {
    "build_engine"
}
$buildWindows = Join-Path $PSScriptRoot "phase2\$buildName"
$build = Convert-ToMsysPath $buildWindows

$env:Path = "$devkitRoot\devkitA64\bin;$devkitRoot\tools\bin;$msysBin;$env:Path"
$env:DEVKITPRO = "/opt/devkitpro"
$env:DEVKITA64 = "/opt/devkitpro/devkitA64"

$configureArgs = @(
    "-S", $source,
    "-B", $build,
    "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.13"
)
if ($Lightweight) { $configureArgs += "-DSWITIFY_LIGHTWEIGHT_OVERLAY=ON" }
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "Phase 2 engine configuration failed" }

& $ninja "-C" $build "-j$Jobs" "switify-background-package"
if ($LASTEXITCODE -ne 0) { throw "Phase 2 engine build failed" }

$overlaySource = Convert-ToMsysPath (Join-Path $PSScriptRoot "phase2\overlay")
$overlayTarget = if ($Lightweight) {
    "switify-overlay-lightweight"
} else {
    "switify-overlay"
}
$overlayBuild = if ($Lightweight) { "build_lightweight" } else { "build" }
$overlayArgs = @(
    "-C", $overlaySource,
    "-j$Jobs",
    "TARGET=$overlayTarget",
    "BUILD=$overlayBuild"
)
if ($Lightweight) { $overlayArgs += "LIGHTWEIGHT=1" }
& $make @overlayArgs
if ($LASTEXITCODE -ne 0) { throw "Switify overlay build failed" }

$phase2Windows = Join-Path $PSScriptRoot "phase2"
$distName = if ($Lightweight) {
    "dist_engine_lightweight"
} else {
    "dist_engine"
}
$dist = Join-Path $phase2Windows $distName
$phase2Resolved = (Resolve-Path -LiteralPath $phase2Windows).Path
if (Test-Path -LiteralPath $dist) {
    $distResolved = (Resolve-Path -LiteralPath $dist).Path
    $expectedPrefix = $phase2Resolved.TrimEnd('\') + '\'
    if (-not $distResolved.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected output path: $distResolved"
    }
    Remove-Item -LiteralPath $distResolved -Recurse -Force
}
$titleRoot = Join-Path $dist "atmosphere\contents\4200000000535754"
$overlayRoot = Join-Path $dist "switch\.overlays"
New-Item -ItemType Directory -Force -Path $titleRoot | Out-Null
New-Item -ItemType Directory -Force -Path $overlayRoot | Out-Null
Copy-Item -Force (Join-Path $buildWindows "switify-background.nsp") `
    (Join-Path $titleRoot "exefs.nsp")
Copy-Item -Force (Join-Path $phase2Windows "sysmodule\toolbox.json") `
    (Join-Path $titleRoot "toolbox.json")
Copy-Item -Force (Join-Path $phase2Windows "overlay\$overlayTarget.ovl") `
    (Join-Path $overlayRoot "Switify.ovl")
$engineZip = Join-Path $phase2Windows "Switify-Phase2-engine-$engineVersion.zip"
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $engineZip -Force

Write-Host ""
Write-Host "Background-engine candidate built:"
Write-Host (Join-Path $buildWindows "switify-background.nsp")
Write-Host "Candidate SD tree: $dist"
Write-Host "Candidate zip: $engineZip"
Write-Host "Package is dormant at boot and starts automatically with Switify.ovl."
