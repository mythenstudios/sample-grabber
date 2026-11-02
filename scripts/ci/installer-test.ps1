# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Installer self test (CI, windows-2022): silent per-user install, check files and registry, silent uninstall, check cleanup.
# Per-user (/CURRENTUSER) so nothing lands in Program Files of the runner and the VST3 goes to the per-user VST3 folder.
param(
    [Parameter(Mandatory = $true)][string]$Setup
)

$ErrorActionPreference = "Stop"
$fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:fail++ }
}

$local   = $env:LOCALAPPDATA
$appDir  = Join-Path $local "Programs\Sample Grabber"
$vst3    = Join-Path $local "Programs\Common\VST3\Sample Grabber.vst3"
$regKey  = "HKCU:\Software\Mythen Studios\Sample Grabber"
$sgLocal = Join-Path $local "SampleGrabber"
$cache   = Join-Path $sgLocal "cache"
$bin     = Join-Path $sgLocal "bin"
$roaming = Join-Path $env:APPDATA "SampleGrabber"
$log     = Join-Path $env:RUNNER_TEMP "sg-setup.log"

Write-Host "---- install ($Setup)"
$p = Start-Process -FilePath $Setup -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/LOG=`"$log`"" -Wait -PassThru
Check ($p.ExitCode -eq 0) "setup exit code $($p.ExitCode)"

Check (Test-Path (Join-Path $appDir "Sample Grabber.exe")) "app in $appDir"
Check (Test-Path (Join-Path $appDir "LICENSE")) "LICENSE next to the app"
Check (Test-Path (Join-Path $vst3 "Contents\x86_64-win\Sample Grabber.vst3")) "VST3 in $vst3"
$listing = Join-Path $appDir "INSTALLED-FILES.txt"
Check (Test-Path $listing) "INSTALLED-FILES.txt"
if (Test-Path $listing) { Get-Content $listing | ForEach-Object { Write-Host "      $_" } }

$reg = Get-ItemProperty -Path $regKey -ErrorAction SilentlyContinue
Check ($null -ne $reg) "registry key"
if ($reg) {
    Check ($reg.CacheDir -eq $cache) "CacheDir = $($reg.CacheDir)"
    Check ($reg.ExportDir -like "*\Sample Grabber") "ExportDir = $($reg.ExportDir)"
    Check ($reg.Vst3Dir -eq (Split-Path $vst3)) "Vst3Dir = $($reg.Vst3Dir)"
}

# What the app would have created: tools, cache (with marker + a foreign file), settings, one export
New-Item -ItemType Directory -Force -Path $bin, (Join-Path $cache "downloads"), $roaming | Out-Null
Set-Content (Join-Path $bin "yt-dlp.exe") "x"
Set-Content (Join-Path $cache ".sg-cache") ""
Set-Content (Join-Path $cache "downloads\song_abcdefghijk.wav") "x"
Set-Content (Join-Path $cache "not-ours.txt") "x"
Set-Content (Join-Path $roaming "settings.xml") "<x/>"
$export = $reg.ExportDir
New-Item -ItemType Directory -Force -Path (Join-Path $export "Song") | Out-Null
Set-Content (Join-Path $export "Song\Song_pad01_0123abcd.wav") "x"

Write-Host "---- uninstall"
$unins = Join-Path $appDir "unins000.exe"
Check (Test-Path $unins) "uninstaller present"
Start-Process -FilePath $unins -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -Wait
# The uninstaller re-launches itself from %TEMP% (_unins*.tmp) and does the post-uninstall cleanup there.
# Inno deletes unins000.exe last, so wait until it is gone and no uninstaller process is left.
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    $running = Get-Process | Where-Object { $_.ProcessName -like "unins*" -or $_.ProcessName -like "_unins*" }
    if (-not (Test-Path $unins) -and -not $running) { break }
    Start-Sleep -Milliseconds 500
}
Check (-not (Test-Path $unins)) "uninstaller finished"

Check (-not (Test-Path (Join-Path $appDir "Sample Grabber.exe"))) "app removed"
Check (-not (Test-Path $vst3)) "VST3 removed"
Check (-not (Test-Path $regKey)) "registry key removed"
Check (-not (Test-Path $bin)) "tools removed"
Check (-not (Test-Path (Join-Path $cache "downloads"))) "cache downloads removed"
Check (-not (Test-Path (Join-Path $cache ".sg-cache"))) "cache marker removed"
Check (Test-Path (Join-Path $cache "not-ours.txt")) "foreign file in cache folder kept"
Check (-not (Test-Path $roaming)) "settings removed"
Check (Test-Path (Join-Path $export "Song\Song_pad01_0123abcd.wav")) "exports kept (silent uninstall never deletes them)"

if ($fail -gt 0) {
    Write-Host "---- setup log"
    if (Test-Path $log) { Get-Content $log -Tail 80 }
    throw "installer test: $fail check(s) failed"
}
Write-Host "installer test: ALL OK"
