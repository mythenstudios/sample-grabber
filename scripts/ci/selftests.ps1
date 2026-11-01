# Runs the app's built-in self tests on Windows (CI). All app folders point into a scratch directory.
# Usage: selftests.ps1 -App <path to Sample Grabber.exe> -Work <scratch dir>
# Exit code 0 = all tests passed. Logs and snapshot PNGs end up in <Work>/out.
param(
    [Parameter(Mandatory)] [string] $App,
    [Parameter(Mandatory)] [string] $Work
)

$ErrorActionPreference = "Stop"
$App = (Resolve-Path $App).Path
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$out = Join-Path $Work "out"
$logs = Join-Path $out "logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$wav = Join-Path $Work "loop124.wav"
python "$PSScriptRoot/make-test-wav.py" $wav 20
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $wav)) { throw "could not create the test wav" }

$tools = Join-Path $Work "tools"
$results = [System.Collections.Generic.List[object]]::new()

# Runs the app once with the given variables, waits (with timeout), prints stderr, records the result
function Invoke-SelfTest([string] $Name, [hashtable] $Vars, [int] $TimeoutSec = 120, [scriptblock] $Check = $null)
{
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
Invoke-SelfTest "state"  @{ SG_STATE_SELFTEST = $wav; SG_SELFTEST_TMP = (Join-Path $Work "state\tmp") }
Invoke-SelfTest "export" @{ SG_EXPORT_SELFTEST = $wav }

# Windows process tree: quoting round trip, kill() and destroying a running process leave nothing behind
Invoke-SelfTest "proc" @{ SG_PROC_SELFTEST = "1" } -TimeoutSec 120

# Needs the network (GitHub releases), installs yt-dlp/deno/ffmpeg into $tools for the grab tests
Invoke-SelfTest "tools" @{ SG_TOOLS_SELFTEST = "1" } -TimeoutSec 900

# Local file only: YouTube blocks CI addresses. Runs ffmpeg through the job object process code.
Invoke-SelfTest "grab-file"  @{ SG_GRAB_SELFTEST = $wav } -TimeoutSec 300
Invoke-SelfTest "grab-range" @{ SG_GRAB_SELFTEST = $wav; SG_GRAB_RANGE = "0:02-0:08" } -TimeoutSec 300

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
