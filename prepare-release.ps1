param(
    [int]$Jobs = 4,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$appVersion = "0.6.4"
$overlayVersion = "0.4.4"
$projectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$releaseRoot = Join-Path $projectRoot "release"

function Assert-ReleaseChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $candidate = [System.IO.Path]::GetFullPath($Path)
    $parent = [System.IO.Path]::GetFullPath($releaseRoot).TrimEnd('\') + '\'
    if (-not $candidate.StartsWith(
            $parent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the release directory: $candidate"
    }
    return $candidate
}

function Reset-ReleaseDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $safePath = Assert-ReleaseChild $Path
    if (Test-Path -LiteralPath $safePath) {
        Remove-Item -LiteralPath $safePath -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $safePath | Out-Null
    return $safePath
}

if (-not $SkipBuild) {
    & (Join-Path $projectRoot "build-app.ps1") -Jobs $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Standalone NRO build failed" }
    & (Join-Path $projectRoot "build-phase2-engine.ps1") -Jobs $Jobs
    if ($LASTEXITCODE -ne 0) { throw "Overlay/background build failed" }
}

$nro = Join-Path $projectRoot "build_app\Switify.nro"
$overlayZip = Join-Path $projectRoot (
    "phase2\Switify-Phase2-engine-{0}.zip" -f $overlayVersion)
foreach ($required in @($nro, $overlayZip)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required release input is missing: $required"
    }
}

$expectedReleaseRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $projectRoot "release"))
if (Test-Path -LiteralPath $releaseRoot) {
    $resolvedReleaseRoot = (Resolve-Path -LiteralPath $releaseRoot).Path
    if (-not $resolvedReleaseRoot.Equals(
            $expectedReleaseRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected release path: $resolvedReleaseRoot"
    }
    # Explorer and antivirus scanners can transiently hold a previously built
    # ZIP open. Clean each generated child independently so one historical
    # locked archive cannot prevent the current release from being staged.
    Get-ChildItem -LiteralPath $resolvedReleaseRoot -Force | ForEach-Object {
        $artifact = $_.FullName
        try {
            Remove-Item -LiteralPath $artifact -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning "Could not remove locked old release artifact: $artifact"
        }
    }
}
New-Item -ItemType Directory -Force -Path $expectedReleaseRoot | Out-Null

# User-facing standalone installation package.
$appStage = Reset-ReleaseDirectory (Join-Path $releaseRoot ".app-stage")
$appInstall = Join-Path $appStage "switch\Switify"
New-Item -ItemType Directory -Force -Path $appInstall | Out-Null
Copy-Item -LiteralPath $nro -Destination (Join-Path $appInstall "Switify.nro")
$appZip = Join-Path $releaseRoot ("Switify-{0}.zip" -f $appVersion)
Compress-Archive -Path (Join-Path $appStage "*") -DestinationPath $appZip -Force
Remove-Item -LiteralPath $appStage -Recurse -Force

# User-facing overlay/background package. The build script already creates the
# correct SD-root layout, so retain it byte-for-byte under the public name.
$publicOverlayZip = Join-Path $releaseRoot (
    "Switify-Overlay-{0}.zip" -f $overlayVersion)
Copy-Item -LiteralPath $overlayZip -Destination $publicOverlayZip -Force

# Clean, buildable source snapshot. Copy only product sources and their exact
# vendored dependencies; development logs, old candidates, SD data, build
# trees, crash artifacts, and the private handover never enter this tree.
$sourceName = "Switify-source-{0}" -f $appVersion
$sourceStage = Reset-ReleaseDirectory (Join-Path $releaseRoot $sourceName)
foreach ($directory in @("app", "targets", "vendor")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $directory) `
        -Destination (Join-Path $sourceStage $directory) -Recurse
}
foreach ($directory in @(".github", "images", "LICENSES")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $directory) `
        -Destination (Join-Path $sourceStage $directory) -Recurse
}
$phase2Target = Join-Path $sourceStage "phase2"
New-Item -ItemType Directory -Force -Path $phase2Target | Out-Null
foreach ($directory in @("common", "ipc-client", "overlay", "probe",
                          "sysmodule", "tests")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "phase2\$directory") `
        -Destination (Join-Path $phase2Target $directory) -Recurse
}
foreach ($file in @("phase2\Makefile", "phase2\README.md",
                     "phase2\THIRD_PARTY_NOTICES.md")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) `
        -Destination $phase2Target
}
foreach ($file in @(".gitattributes", ".gitignore", "CHANGELOG.md",
                     "BUILDING.md", "CONTRIBUTING.md", "LICENSE", "README.md",
                     "RELEASE_NOTES.md", "SECURITY.md",
                     "THIRD_PARTY_NOTICES.md", "build-app.ps1",
                     "build-phase2-engine.ps1", "deploy-app.ps1",
                     "prepare-release.ps1", "resolve-spotify-hashes.py",
                     "symbolicate-crash.ps1")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) `
        -Destination $sourceStage
}

foreach ($generatedDirectory in @(
        "phase2\overlay\build", "phase2\probe\build",
        "phase2\sysmodule\build")) {
    $path = Join-Path $sourceStage $generatedDirectory
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
$sourceMetadataDirectories = @(Get-ChildItem -LiteralPath $sourceStage `
    -Recurse -Force -Directory | Where-Object {
        $_.Name -in @(".git", ".idea", ".vscode")
    })
foreach ($directory in $sourceMetadataDirectories) {
    if (Test-Path -LiteralPath $directory.FullName) {
        Remove-Item -LiteralPath $directory.FullName -Recurse -Force
    }
}
$generatedExtensions = @(".elf", ".nacp", ".npdm", ".nro", ".nso", ".nsp",
                         ".ovl", ".map")
Get-ChildItem -LiteralPath $sourceStage -Recurse -File | Where-Object {
    $generatedExtensions -contains $_.Extension -or $_.Name -like "*.log"
} | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

$privateFiles = @(Get-ChildItem -LiteralPath $sourceStage -Recurse -File |
    Where-Object {
        $_.Name -in @("credentials.json", "playback-session.json",
                       "app-settings.json", "config.json")
    })
if ($privateFiles.Count -ne 0) {
    throw "Private runtime files entered the source package: $($privateFiles.FullName -join ', ')"
}

$sourceZip = Join-Path $releaseRoot ("{0}.zip" -f $sourceName)
Compress-Archive -Path $sourceStage -DestinationPath $sourceZip -Force
Remove-Item -LiteralPath $sourceStage -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "RELEASE_NOTES.md") `
    -Destination (Join-Path $releaseRoot "RELEASE_NOTES.md") -Force

$hashTargets = @($appZip, $publicOverlayZip, $sourceZip)
$hashLines = foreach ($file in $hashTargets) {
    $hash = Get-FileHash -LiteralPath $file -Algorithm SHA256
    "{0}  {1}" -f $hash.Hash.ToLowerInvariant(), (Split-Path $file -Leaf)
}
$checksums = Join-Path $releaseRoot "SHA256SUMS.txt"
Set-Content -LiteralPath $checksums -Value $hashLines -Encoding ascii

Write-Host ""
Write-Host "Release staging complete: $releaseRoot"
$hashTargets + $checksums | ForEach-Object { Write-Host $_ }
