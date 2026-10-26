# Install NFSU2 Co-op into a game folder.
#
#   .\install.ps1 -GameDir "C:\Games\NFS U2 coop mod\game"
#   .\install.ps1 -GameDir "...\game" -Force     # overwrite the existing ini
#
# Only two files are added: scripts\NFSU2Coop.asi and scripts\nfsu2coop.ini.
# Nothing in the game exe, archives, saves or other mods is modified.
param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$asi = Join-Path $root 'build\NFSU2Coop.asi'
if (-not (Test-Path $asi)) { throw "build\NFSU2Coop.asi not found. Run .\build.ps1 first." }
if (-not (Test-Path (Join-Path $GameDir 'speed2.exe'))) {
    throw "'$GameDir' does not look like a Need for Speed Underground 2 folder (speed2.exe missing)."
}

$scripts = Join-Path $GameDir 'scripts'
New-Item -ItemType Directory -Force -Path $scripts | Out-Null

$targetAsi = Join-Path $scripts 'NFSU2Coop.asi'
Copy-Item $asi $targetAsi -Force
Write-Host "installed $targetAsi"

$ini = Join-Path $scripts 'nfsu2coop.ini'
if ((Test-Path $ini) -and -not $Force) {
    Write-Host "kept existing $ini (use -Force to replace it with defaults)"
} else {
    Copy-Item (Join-Path $root 'nfsu2coop.default.ini') $ini -Force
    Write-Host "installed $ini"
}

$asiLoader = Join-Path $GameDir 'dinput8.dll'
if (-not (Test-Path $asiLoader)) {
    Write-Warning "no dinput8.dll in $GameDir - install Ultimate ASI Loader, or the .asi will not load."
} else {
    Write-Host "ASI loader present: dinput8.dll"
}

Write-Host ""
Write-Host "next:"
Write-Host "  1. edit $ini  (Host=1 to host, Server=<ip> to join)"
Write-Host "  2. start speed2.exe"
Write-Host "  3. in game: F6 opens event, F5 ready, F8 toggles the HUD"
Write-Host "  4. log: $scripts\NFSU2Coop-<pid>.log"

