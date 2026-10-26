# NFSU2 Co-op build. Uses the portable i686 MinGW-w64 from toolchain/ (space-free
# path required by GCC itself: see docs/BUILD.md).
#
#   .\build.ps1                 # everything: server, bots, tests, probe, plugin
#   .\build.ps1 -Targets core   # just check the core compiles into a static lib
#   .\build.ps1 -Clean          # wipe build\
param(
    [string[]]$Targets = @('all'),
    [switch]$Clean,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tc = if ($env:NFSU2_TOOLCHAIN) { $env:NFSU2_TOOLCHAIN } else { 'C:\nfsu2-toolchain' }
$gxx = Join-Path $tc 'bin\g++.exe'
$gcc = Join-Path $tc 'bin\gcc.exe'
$ar = Join-Path $tc 'bin\ar.exe'

if (-not (Test-Path $gxx)) {
    throw "MinGW not found at $tc. Run toolchain\install.ps1, or set NFSU2_TOOLCHAIN."
}
if ($Targets -contains 'all') { $Targets = @('core', 'apps', 'tests', 'plugin') }

$build = Join-Path $root 'build'
$objDir = Join-Path $build 'obj'
$lib = Join-Path $build 'libnfc.a'

if ($Clean -and (Test-Path $build)) {
    $resolved = (Resolve-Path $build).Path
    if (-not $resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to clean outside the project: $resolved"
    }
    Remove-Item $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$inc = @(
    (Join-Path $root 'src'),
    (Join-Path $root 'vendor\enet\include'),
    (Join-Path $root 'vendor\minhook\include'),
    (Join-Path $root 'vendor\imgui'),
    (Join-Path $root 'vendor\imgui\backends')
) | ForEach-Object { "-I$_" }

$defs = @('-D_WIN32_WINNT=0x0601', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS')
$cxxFlags = @('-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers', '-MMD', '-MP') + $defs + $inc
$cFlags = @('-std=c99', '-O2', '-g', '-Wall', '-Wno-unused-parameter', '-MMD', '-MP') + $defs + $inc

function Get-Newest($paths) {
    $newest = [datetime]::MinValue
    foreach ($p in $paths) {
        if (Test-Path $p) {
            Get-ChildItem $p -Recurse -File -Include *.h, *.hpp, *.c, *.cpp, *.rc |
                ForEach-Object { if ($_.LastWriteTime -gt $newest) { $newest = $_.LastWriteTime } }
        }
    }
    return $newest
}

$headerStamp = Get-Newest @((Join-Path $root 'src'), (Join-Path $root 'vendor\enet\include'), (Join-Path $root 'vendor\minhook\include'))

function Compile-One($src, $extraFlags) {
    $rel = $src.Substring($root.Length).TrimStart('\', '/') -replace '[\\/]', '_' -replace '\.(c|cpp)$', ''
    $out = Join-Path $objDir ($rel + '.o')
    $needs = $true
    if (Test-Path $out) {
        $o = Get-Item $out
        $s = Get-Item $src
        if ($o.LastWriteTime -ge $s.LastWriteTime -and $o.LastWriteTime -ge $headerStamp) { $needs = $false }
    }
    if ($needs) {
        if ($Verbose) { Write-Host "CC $src" }
        $compiler = if ($src -match '\.c$') { $gcc } else { $gxx }
        $flags = if ($src -match '\.c$') { $cFlags } else { $cxxFlags }
        & $compiler @flags @extraFlags -c $src -o $out
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    }
    return $out
}

$coreSrc = Get-ChildItem (Join-Path $root 'src\core') -Filter *.cpp | ForEach-Object { $_.FullName }
$enetSrc = @('callbacks.c', 'compress.c', 'host.c', 'list.c', 'packet.c', 'peer.c', 'protocol.c', 'win32.c') |
    ForEach-Object { Join-Path $root "vendor\enet\$_" }

$targetsCore = @($coreSrc + $enetSrc)
if ($Targets -contains 'core' -or $Targets -contains 'apps' -or $Targets -contains 'tests' -or $Targets -contains 'plugin') {
    $objs = @()
    foreach ($s in $targetsCore) { $objs += Compile-One $s @() }
    if (Test-Path $lib) { Remove-Item $lib -Force }
    & $ar rcs $lib @objs
    if ($LASTEXITCODE -ne 0) { throw "ar failed" }
    Write-Host "OK core -> build\libnfc.a ($($objs.Count) objects)"
}

function Link-Exe($name, $sources) {
    $objs = @()
    foreach ($s in $sources) { $objs += Compile-One $s @() }
    $out = Join-Path $build "$name.exe"
    $linkFlags = @('-o', $out) + $objs + @($lib, '-lws2_32', '-lwinmm', '-static', '-static-libgcc', '-static-libstdc++', '-Wl,-s')
    & $gxx @linkFlags
    if ($LASTEXITCODE -ne 0) { throw "link failed: $name" }
    Write-Host "OK $name -> build\$name.exe"
    return $out
}

if ($Targets -contains 'apps') {
    Link-Exe 'coopd' @((Join-Path $root 'src\apps\coopd.cpp')) | Out-Null
    Link-Exe 'coop-bot' @((Join-Path $root 'src\apps\coopbot.cpp')) | Out-Null
    Link-Exe 'coop-probe' @((Join-Path $root 'src\apps\probe.cpp'), (Join-Path $root 'src\game\scanner.cpp')) | Out-Null
}

if ($Targets -contains 'tests') {
    Link-Exe 'core-tests' @((Join-Path $root 'tests\core_tests.cpp')) | Out-Null
    Link-Exe 'sync-tests' @((Join-Path $root 'tests\sync_tests.cpp')) | Out-Null
    Link-Exe 'net-tests' @((Join-Path $root 'tests\net_tests.cpp')) | Out-Null
}

if ($Targets -contains 'plugin') {
    $src = @(
        (Join-Path $root 'src\plugin\main.cpp'),
        (Join-Path $root 'src\game\config.cpp'),
        (Join-Path $root 'src\game\nfsu2_bindings.cpp'),
        (Join-Path $root 'src\game\scanner.cpp'),
        (Join-Path $root 'src\game\overlay.cpp'),
        (Join-Path $root 'vendor\imgui\imgui.cpp'),
        (Join-Path $root 'vendor\imgui\imgui_draw.cpp'),
        (Join-Path $root 'vendor\imgui\imgui_tables.cpp'),
        (Join-Path $root 'vendor\imgui\imgui_widgets.cpp'),
        (Join-Path $root 'vendor\imgui\backends\imgui_impl_dx9.cpp'),
        (Join-Path $root 'vendor\minhook\src\buffer.c'),
        (Join-Path $root 'vendor\minhook\src\hook.c'),
        (Join-Path $root 'vendor\minhook\src\trampoline.c'),
        (Join-Path $root 'vendor\minhook\src\hde\hde32.c'),
        (Join-Path $root 'vendor\minhook\src\hde\hde64.c')
    )
    $objs = @()
    foreach ($s in $src) { $objs += Compile-One $s @() }
    $out = Join-Path $build 'NFSU2Coop.asi'
    $linkFlags = @('-shared', '-o', $out) + $objs + @($lib, '-lws2_32', '-ld3d9', '-ldxguid', '-luser32', '-lgdi32', '-lwinmm', '-lole32', '-static', '-static-libgcc', '-static-libstdc++', '-Wl,-s')
    & $gxx @linkFlags
    if ($LASTEXITCODE -ne 0) { throw "link failed: NFSU2Coop.asi" }
    Write-Host "OK plugin -> build\NFSU2Coop.asi"
}

Write-Host "done."

