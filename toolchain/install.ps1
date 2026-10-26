# Installs everything the project builds and tests with.
#
#   .\toolchain\install.ps1              # install what is missing
#   .\toolchain\install.ps1 -Force       # re-download and re-extract
#   .\toolchain\install.ps1 -Only mingw
#
# Pinned by SHA-256 from toolchain/catalog.yaml. Nothing is piped to a shell.
param(
    [string]$Prefix = 'C:\nfsu2-toolchain',
    [ValidateSet('all', 'mingw', 'vendor', 'python')]
    [string]$Only = 'all',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root
$cache = Join-Path $root 'cache'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

$tools = @(
    @{ id = 'mingw'; url = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-i686-posix-dwarf-gcc-16.2.0-llvm-23.1.3-mingw-w64ucrt-14.0.0-r3.zip';
       sha = '9353F386F7A678AB41B0746DB43211C4B6B8AB2811D2EDB289F0496711FB1C9B'; file = 'winlibs-i686.zip'; inner = 'mingw32'; vendor = $false },
    @{ id = 'enet'; url = 'https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.zip';
       sha = '32D6B15611C2667B88CD865E0CB48C3C011DDEFCFF6DFBDB266B8E0B04FFFAB3'; file = 'enet.zip'; inner = 'enet-1.3.18'; vendor = $true },
    @{ id = 'minhook'; url = 'https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip';
       sha = '172708123DAA0C98D20D3A980B16A50BE14AF243DC95DEE6F79C24193AD010E4'; file = 'minhook.zip'; inner = 'minhook-1.3.4'; vendor = $true },
    @{ id = 'imgui'; url = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip';
       sha = 'FD37507C8476A6D14CC7C4B352401F31BCBD0F0D995D35390811E968C466F46E'; file = 'imgui.zip'; inner = 'imgui-1.91.9b'; vendor = $true }
)

function Get-Pinned($url, $dest, $sha) {
    if ((Test-Path $dest) -and -not $Force) {
        $have = (Get-FileHash $dest -Algorithm SHA256).Hash
        if ($have -eq $sha) { Write-Host "  cached $([IO.Path]::GetFileName($dest))"; return }
        Write-Host "  cached file has the wrong hash, re-downloading"
    }
    Write-Host "  download $url"
    & curl.exe -L --retry 3 --fail -o $dest $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
    $have = (Get-FileHash $dest -Algorithm SHA256).Hash
    if ($have -ne $sha) { throw "SHA-256 mismatch for $dest`n  expected $sha`n  got      $have" }
}

function Expand-To($zip, $inner, $target) {
    $tmp = Join-Path $cache ('tmp_' + [IO.Path]::GetFileNameWithoutExtension($zip) + '_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $tmp)
    $src = Join-Path $tmp $inner
    if (-not (Test-Path $src)) { throw "archive layout changed: $inner not found in $zip" }
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Move-Item $src $target
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

$installed = [ordered]@{ updated = (Get-Date).ToString('s'); prefix = $Prefix; tools = @() }

if ($Only -eq 'all' -or $Only -eq 'mingw') {
    Write-Host '== MinGW-w64 i686 =='
    $m = $tools | Where-Object { $_.id -eq 'mingw' }
    $zip = Join-Path $cache $m.file
    Get-Pinned $m.url $zip $m.sha
    if ($Force -or -not (Test-Path (Join-Path $Prefix 'bin\g++.exe'))) {
        Write-Host "  extract to $Prefix"
        Expand-To $zip $m.inner $Prefix
    } else {
        Write-Host "  present at $Prefix"
    }
    $junction = Join-Path $root 'mingw32'
    if (-not (Test-Path $junction)) {
        New-Item -ItemType Junction -Path $junction -Target $Prefix | Out-Null
        Write-Host "  junction toolchain\mingw32 -> $Prefix"
    }
    $installed.tools += @{ id = 'mingw'; path = $Prefix }
}

if ($Only -eq 'all' -or $Only -eq 'vendor') {
    Write-Host '== vendored sources =='
    foreach ($t in ($tools | Where-Object { $_.vendor })) {
        $target = Join-Path $repo ('vendor\' + $t.id)
        $zip = Join-Path $cache $t.file
        if ((Test-Path $target) -and -not $Force) {
            Write-Host "  vendor\$($t.id) present"
        } else {
            Get-Pinned $t.url $zip $t.sha
            Expand-To $zip $t.inner $target
            Write-Host "  vendor\$($t.id) extracted"
        }
        $installed.tools += @{ id = $t.id; path = $target }
    }
}

if ($Only -eq 'all' -or $Only -eq 'python') {
    Write-Host '== Python test dependencies =='
    & python -m pip install --quiet --upgrade pytest pyyaml pefile
    if ($LASTEXITCODE -ne 0) { throw 'pip install failed' }
    $installed.tools += @{ id = 'python-deps'; path = 'pytest, pyyaml, pefile' }
}

$json = Join-Path $root 'INSTALLED.json'
$installed | ConvertTo-Json -Depth 5 | Set-Content $json -Encoding UTF8
Write-Host ""
Write-Host "toolchain ready. build with:  .\build.ps1"
Write-Host "state written to toolchain\INSTALLED.json"
