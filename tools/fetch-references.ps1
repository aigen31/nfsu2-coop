# Clone the reference projects used to study NFSU2 into ..\references (not part of the build).
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'references'
New-Item -ItemType Directory -Force $dest | Out-Null
$repos = @(
    'https://github.com/th0mk/nfsu2-web',
    'https://github.com/codepdbh/nfsu2-recompiled',
    'https://github.com/nlgxzef/NFSU2Unlimiter',
    'https://github.com/yugecin/nfsu2-re'
)
foreach ($r in $repos) {
    $name = ($r -split '/')[-1]
    $path = Join-Path $dest $name
    if (Test-Path $path) { Write-Host "have $name"; continue }
    git clone --depth 1 $r $path
}
Write-Host "done: $dest"
