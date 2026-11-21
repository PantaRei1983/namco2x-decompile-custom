# Automatic check of a 4-cabinet Rave Racer link on this PC.
# Four copies run hidden (no windows, so the graphics chip cannot make them stutter), linked over this PC's own
# network loopback. Each one puts a coin in, starts, chooses linked play and drives a race by itself.
# Afterwards each cabinet's log is read and the result is shown as PASS or FAIL.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path (Split-Path $PSScriptRoot -Parent) 'games\Rave Racer')   # the game's folder: its program, roms and logs
$seconds = 100
$frames = [int]($seconds * 60)

Write-Host "RAVE RACER - AUTOMATIC 4-CABINET LINK CHECK"
Write-Host "==========================================="
Write-Host "Four hidden copies of the game link up and race each other for $seconds seconds."
Write-Host "Please wait; do not start the game yourself until this has finished."
Write-Host ""

1..4 | ForEach-Object { Remove-Item "raveracer_cab$_.log" -ErrorAction SilentlyContinue }
$procs = 1..4 | ForEach-Object {
    Start-Process -FilePath ".\RaveRacer-ArcadeLAN.exe" -WindowStyle Hidden -PassThru -ArgumentList @(
        'roms', '--frames', $frames, '--replay', '..\..\tools\rr_race.rep', '--link', $_, '--link-cabinets', '4', '--mute')
}
$start = Get-Date
while ($procs | Where-Object { -not $_.HasExited }) {
    $t = [int]((Get-Date) - $start).TotalSeconds
    Write-Progress -Activity "Four cabinets racing" -Status "$t of about $($seconds + 5) seconds" -PercentComplete ([Math]::Min(100, 100 * $t / ($seconds + 5)))
    Start-Sleep -Seconds 1
    if ($t -gt $seconds + 90) { $procs | Where-Object { -not $_.HasExited } | Stop-Process -Force; Write-Host "(stopped: it took far too long)" }
}
Write-Progress -Activity "Four cabinets racing" -Completed

$pass = $true
foreach ($c in 1..4) {
    $log = "raveracer_cab$c.log"
    if (-not (Test-Path $log)) { Write-Host "Cabinet ${c}: FAIL - it wrote no log (did it start?)" -ForegroundColor Red; $pass = $false; continue }
    $lines = Get-Content $log
    $linked = $lines | Where-Object { $_ -match '\[LINK\] linked \(.*cabinets 1, 2, 3, 4,' } | Select-Object -First 1
    $last = $lines | Where-Object { $_ -match '\[LINK\] frame \d+  down' } | Select-Object -Last 1
    $problems = @()
    if (-not $linked) { $problems += "did not link with all four cabinets at start-up" }
    if (-not $last) { $problems += "no link report in its log" }
    else {
        $m = [regex]::Match($last, 'frame (\d+)  down (\d+).* lost (\d+).*\| game: cabinet (\d+), hears \w+ \((\d+) in the ring\), link (\w+)')
        $down = [int]$m.Groups[2].Value; $lost = [int]$m.Groups[3].Value; $ring = [int]$m.Groups[5].Value
        if ($m.Groups[6].Value -ne 'GOOD') { $problems += "the game says its link is down at the end" }
        if ($ring -ne 4) { $problems += "the game hears $ring cabinets, not 4" }
        if ($down -gt 0) { $problems += "the game lost the link for $down frames during the race" }
        $detail = "game ID $($m.Groups[4].Value), hears $ring cabinets, link $($m.Groups[6].Value), link lost for $down frames, network packets lost $lost"
    }
    if ($lines -match 'WARNING') { $problems += (($lines -match 'WARNING') | Select-Object -First 1) }
    if ($problems.Count -eq 0) { Write-Host "Cabinet ${c}: PASS - $detail" -ForegroundColor Green }
    else { $pass = $false; Write-Host "Cabinet ${c}: FAIL - $($problems -join '; ')" -ForegroundColor Red; if ($detail) { Write-Host "           ($detail)" } }
    $detail = $null
}
Write-Host ""
if ($pass) { Write-Host "RESULT: PASS - all four cabinets linked at start-up and stayed linked for the whole race." -ForegroundColor Green }
else { Write-Host "RESULT: FAIL - send the four raveracer_cab*.log files from games\Rave Racer." -ForegroundColor Red }
