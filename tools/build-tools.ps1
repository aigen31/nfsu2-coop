# Build the reverse-engineering helpers (not part of the shipped mod).
#
#   .\tools\build-tools.ps1
#
# Produces:
#   build\recon.exe          read-only memory reconnaissance (pid, scan, model)
#   build\tap.dll            injected probe that logs CarRenderInfo::Render
#   build\coop-inject.exe    LoadLibrary injector for tap.dll
#
# They are how the [Bindings] values in nfsu2coop.ini were found on the verified
# build. They never write to the game except when you explicitly pass a value.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tc = if ($env:NFSU2_TOOLCHAIN) { $env:NFSU2_TOOLCHAIN } else { 'C:\nfsu2-toolchain' }
$gcc = Join-Path $tc 'bin\gcc.exe'
$gxx = Join-Path $tc 'bin\g++.exe'
$build = Join-Path $root 'build'
$obj = Join-Path $build 'tools'
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$mh = @()
foreach ($f in 'buffer', 'hook', 'trampoline') {
    & $gcc -O2 -c (Join-Path $root "vendor\minhook\src\$f.c") -o (Join-Path $obj "mh_$f.o") -I (Join-Path $root 'vendor\minhook\include')
    $mh += (Join-Path $obj "mh_$f.o")
}
foreach ($f in 'hde32', 'hde64') {
    & $gcc -O2 -c (Join-Path $root "vendor\minhook\src\hde\$f.c") -o (Join-Path $obj "mh_$f.o") -I (Join-Path $root 'vendor\minhook\include')
    $mh += (Join-Path $obj "mh_$f.o")
}

& $gxx -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -o (Join-Path $build 'recon.exe') (Join-Path $root 'tools\recon.cpp')
if ($LASTEXITCODE -ne 0) { throw 'recon build failed' }

& $gxx -O2 -shared -o (Join-Path $build 'tap.dll') (Join-Path $root 'tools\tap.cpp') @mh `
    -I (Join-Path $root 'vendor\minhook\include') -static -static-libgcc -static-libstdc++ -luser32
if ($LASTEXITCODE -ne 0) { throw 'tap build failed' }

& $gxx -O2 -static -o (Join-Path $build 'coop-inject.exe') (Join-Path $root 'tools\inject.cpp')
if ($LASTEXITCODE -ne 0) { throw 'inject build failed' }

Write-Host 'OK recon.exe, tap.dll, coop-inject.exe -> build\'
