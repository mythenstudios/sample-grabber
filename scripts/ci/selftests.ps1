# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Runs the app's built-in self tests in CI (Windows, macOS). All app folders point into a scratch directory.
# Usage: selftests.ps1 -App <path to the app executable> -Work <scratch dir> [-Skip name,name]
# Exit code 0 = all tests passed. Logs and snapshot PNGs end up in <Work>/out.
param(
    [Parameter(Mandatory)] [string] $App,
    [Parameter(Mandatory)] [string] $Work,
    [string[]] $Skip = @()   # e.g. tools,grab-file,grab-range where the tool installer is not supported yet
)

$ErrorActionPreference = "Stop"
$App = (Resolve-Path $App).Path
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$out = Join-Path $Work "out"
$logs = Join-Path $out "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$wav = Join-Path $Work "loop124.wav"
$py = if ($IsWindows) { "python" } else { "python3" }   # python3 on Windows may be the Microsoft Store stub
& $py "$PSScriptRoot/make-test-wav.py" $wav 20
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $wav)) { throw "could not create the test wav" }

$tools = Join-Path $Work "tools"
$results = [System.Collections.Generic.List[object]]::new()

# Runs the app once with the given variables, waits (with timeout), prints stderr, records the result
function Invoke-SelfTest([string] $Name, [hashtable] $Vars, [int] $TimeoutSec = 120, [scriptblock] $Check = $null)
{
    if ($Skip -contains $Name) { Write-Host "skipped: $Name"; return }
    $base = Join-Path $Work $Name
    $all = @{
        SG_CACHE_DIR  = Join-Path $base "cache"
        SG_EXPORT_DIR = Join-Path $base "export"
        SG_TOOLS_DIR  = $tools
    }
    foreach ($k in $Vars.Keys) { $all[$k] = $Vars[$k] }
    foreach ($k in $all.Keys) { Set-Item "env:$k" $all[$k] }

    $err = Join-Path $logs "$Name.log"
    $stdout = Join-Path $logs "$Name.stdout.log"
    Write-Host "::group::$Name"
    $t0 = Get-Date
    $p = Start-Process -FilePath $App -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput $stdout
    $null = $p.Handle   # keeps the exit code available after the process ends
    $finished = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) { $p.Kill($true); $p.WaitForExit() }
    $secs = ((Get-Date) - $t0).TotalSeconds

    foreach ($k in $all.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path $err) { Get-Content $err | Write-Host }

    $code = if ($finished) { $p.ExitCode } else { "timeout" }
    $ok = $finished -and $p.ExitCode -eq 0
    $note = ""
    if ($ok -and $Check) { $note = & $Check; if ($note) { $ok = $false } }
    Write-Host "::endgroup::"
    $results.Add([pscustomobject] @{ Test = $Name; Result = $(if ($ok) { "ok" } else { "FAIL" }); Exit = $code
                                     Seconds = [math]::Round($secs, 1); Note = $note })
}

Invoke-SelfTest "range"  @{ SG_RANGE_SELFTEST = "1" }
Invoke-SelfTest "cache"  @{ SG_CACHE_SELFTEST = "1" }
Invoke-SelfTest "state"  @{ SG_STATE_SELFTEST = $wav; SG_SELFTEST_TMP = (Join-Path (Join-Path $Work "state") "tmp") }
Invoke-SelfTest "export" @{ SG_EXPORT_SELFTEST = $wav }

# Windows process tree: quoting round trip, kill() and destroying a running process leave nothing behind
Invoke-SelfTest "proc" @{ SG_PROC_SELFTEST = "1" } -TimeoutSec 120

# Needs the network (GitHub releases), installs yt-dlp/deno/ffmpeg into $tools for the grab tests
Invoke-SelfTest "tools" @{ SG_TOOLS_SELFTEST = "1" } -TimeoutSec 900

# Local file only: YouTube blocks CI addresses. Runs ffmpeg through the job object process code.
Invoke-SelfTest "grab-file"  @{ SG_GRAB_SELFTEST = $wav } -TimeoutSec 300
Invoke-SelfTest "grab-range" @{ SG_GRAB_SELFTEST = $wav; SG_GRAB_RANGE = "0:02-0:08" } -TimeoutSec 300

# Cancel while ffmpeg cuts a long file: no process of the job (whole tree) and no partial file may survive
$longWav = Join-Path $Work "long120.wav"   # pure Python generator: keep it short for the CI runners
& $py "$PSScriptRoot/make-test-wav.py" $longWav 120
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $longWav)) { throw "could not create the long test wav" }
Invoke-SelfTest "grab-cancel" @{ SG_GRAB_SELFTEST = $longWav; SG_GRAB_RANGE = "0:05-1:55"; SG_GRAB_CANCEL_AFTER = "40" } -TimeoutSec 120

foreach ($theme in "dark", "classic")
{
    $shots = Join-Path $out "snapshots"
    Invoke-SelfTest "snapshot-$theme" @{ SG_SNAPSHOT = $shots; SG_THEME = $theme; SG_SNAPSHOT_FILE = $wav } -TimeoutSec 60 -Check {
        $png = Join-Path $shots "$theme.png"
        if (-not (Test-Path $png) -or (Get-Item $png).Length -lt 10000) { "no or empty $theme.png" }
    }.GetNewClosure()
}

$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object Result -ne "ok")
if ($failed.Count -gt 0) { Write-Host "$($failed.Count) self test(s) failed"; exit 1 }
Write-Host "all self tests passed"
exit 0
