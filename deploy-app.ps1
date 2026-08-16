param(
    [Parameter(Mandatory = $true)]
    [string]$Address,
    [int]$Jobs = 4,
    [switch]$SkipBuild,
    [switch]$LiveLogs
)

$ErrorActionPreference = "Stop"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build-app.ps1") -Jobs $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Switify build failed with exit code $LASTEXITCODE."
    }
}

$nro = Join-Path $PSScriptRoot "build_app\Switify.nro"
if (-not (Test-Path -LiteralPath $nro)) {
    throw "Switify.nro was not found. Run without -SkipBuild first."
}

$nxlinkCandidates = @(
    "C:\devkitPro\tools\bin\nxlink.exe"
)
if (-not [string]::IsNullOrWhiteSpace($env:DEVKITPRO) -and
    $env:DEVKITPRO -notmatch '^/opt/devkitpro/?$') {
    $nxlinkCandidates = @(
        (Join-Path $env:DEVKITPRO "tools\bin\nxlink.exe")
    ) + $nxlinkCandidates
}

$nxlink = $nxlinkCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $nxlink) {
    $command = Get-Command nxlink -ErrorAction SilentlyContinue
    if ($command) { $nxlink = $command.Source }
}
if (-not $nxlink) {
    throw "nxlink was not found. Install devkitPro tools or add nxlink to PATH."
}

Write-Host ""
Write-Host "Open NetLoader in HBMenu, then keep that screen active."
Write-Host "Sending $nro to $Address on NetLoader's standard port 28280..."

# nxlink always uses HBMenu's NetLoader protocol/port. Its -p option means the
# upload destination path, not a TCP port, so no port argument belongs here.
if ($LiveLogs) {
    $logDirectory = Join-Path $PSScriptRoot "logs"
    if (-not (Test-Path -LiteralPath $logDirectory)) {
        New-Item -ItemType Directory -Path $logDirectory | Out-Null
    }
    $hostLog = Join-Path $logDirectory ("nxlink-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
    Write-Host "Live logs will remain attached until Switify exits."
    Write-Host "Host copy: $hostLog"
    # Windows PowerShell promotes redirected native stderr to ErrorRecord.
    # Keep it visible/saved without letting ErrorActionPreference=Stop abort
    # before we can report nxlink's actual exit code.
    $previousErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $nxlink -a $Address -r 10 -s $nro 2>&1 |
            ForEach-Object { [string]$_ } |
            Tee-Object -FilePath $hostLog
        $nxlinkExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorPreference
    }
} else {
    & $nxlink -a $Address -r 10 $nro
    $nxlinkExitCode = $LASTEXITCODE
}
if ($nxlinkExitCode -ne 0) {
    throw "NetLoader upload failed with exit code $nxlinkExitCode."
}

Write-Host "Switify was delivered successfully."
