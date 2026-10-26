# Run every test in the project. Prints ALL OK only when all of them pass.
#
#   .\tests\run_all.ps1            # build + unit + sync + net + in-game probe + two-process run
#   .\tests\run_all.ps1 -SkipBuild
param(
    [switch]$SkipBuild,
    [switch]$SkipProcesses
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = Join-Path $root 'build'
$failures = 0

function Step($name, [scriptblock]$body) {
    Write-Host ""
    Write-Host "== $name =="
    $global:LASTEXITCODE = 0
    try {
        & $body
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "exit code $LASTEXITCODE" }
        Write-Host "ok   $name"
    } catch {
        Write-Host "FAIL $name : $_"
        $script:failures++
    }
}

if (-not $SkipBuild) {
    Step 'build' {
        & (Join-Path $root 'build.ps1')
    }
}

Step 'core tests' { & (Join-Path $build 'core-tests.exe') }
Step 'sync tests' { & (Join-Path $build 'sync-tests.exe') }
Step 'net tests'  { & (Join-Path $build 'net-tests.exe') }

$exe = Join-Path (Split-Path -Parent $root) 'game\speed2.exe'
if (Test-Path $exe) {
    Step 'probe: anchors in speed2.exe' {
        & (Join-Path $build 'coop-probe.exe') --exe $exe --json (Join-Path $build 'probe-report.json')
    }
} else {
    Write-Host "skip probe: $exe not found"
}

if (-not $SkipProcesses) {
    Step 'two processes: coopd + 2 bots' {
        $port = 28350
        $outDir = Join-Path $build 'e2e'
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        Remove-Item (Join-Path $outDir '*') -Force -ErrorAction SilentlyContinue

        $d = Start-Process -FilePath (Join-Path $build 'coopd.exe') `
            -ArgumentList '--port', $port, '--no-traffic', '--auto-race', '5', '--countdown', '800', '--seconds', '12' `
            -RedirectStandardOutput (Join-Path $outDir 'coopd.out') `
            -RedirectStandardError (Join-Path $outDir 'coopd.err') -PassThru -NoNewWindow
        Start-Sleep -Milliseconds 400

        $bots = @()
        foreach ($n in @('bot1', 'bot2')) {
            $bots += Start-Process -FilePath (Join-Path $build 'coop-bot.exe') `
                -ArgumentList '--connect', "127.0.0.1:$port", '--name', $n, '--race', '--seconds', '14', '--json', ("`"$(Join-Path $outDir "$n.json")`"") `
                -RedirectStandardOutput (Join-Path $outDir "$n.out") `
                -RedirectStandardError (Join-Path $outDir "$n.err") -PassThru -NoNewWindow
        }
        foreach ($b in $bots) { $b.WaitForExit(20000) | Out-Null }
        $d.WaitForExit(20000) | Out-Null

        foreach ($b in $bots) {
            if ($b.ExitCode -ne 0) { throw "a bot exited with $($b.ExitCode)" }
        }
        $j1 = Get-Content (Join-Path $outDir 'bot1.json') -Raw | ConvertFrom-Json
        $j2 = Get-Content (Join-Path $outDir 'bot2.json') -Raw | ConvertFrom-Json
        if (-not $j1.connected -or -not $j2.connected) { throw 'a bot never connected' }
        if (-not $j1.raceFinished -or -not $j2.raceFinished) { throw 'the race did not finish for both bots' }
        if ($j1.ledgerRevision -ne 1 -or $j2.ledgerRevision -ne 1) { throw "career applied $($j1.ledgerRevision)/$($j2.ledgerRevision) times" }
        if ($j1.progressDigest -ne $j2.progressDigest) { throw 'career digests differ between clients' }
        Write-Host "     both bots finished; career digest $($j1.progressDigest), cash $($j1.cash), rep $($j1.rep)"
    }
}

Write-Host ""
if ($failures -eq 0) {
    Write-Host "ALL OK"
    exit 0
}
Write-Host "$failures STEP(S) FAILED"
exit 1


