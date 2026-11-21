# lan-test.ps1 -- REAL LAN TESTS ACROSS SEVERAL PCS (the .bat files beside this start it; see "About the LAN test.txt").
#   -Share   (main PC, as administrator) make the results folder and share it on the network as N22LANTEST
#   -Run     (every PC) pick a game and this PC's cabinet number; the link is set up for the test (Automatic finding), the game
#            runs full screen, and when it is closed its logs go to the results folder; the game's own settings are put back
#   -Check   (main PC) a short report of every test in the results folder
# Written for Windows PowerShell 5.1 (every Windows 10 / 11 PC has it).
param([switch]$Share, [switch]$Run, [switch]$Check)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kit = Split-Path -Parent $Here
$CfgFile = Join-Path $Here 'lan-test.cfg'
$ShareName = 'N22LANTEST'
$LocalResults = Join-Path $Here 'results'

# the LAN games: folder, program, arguments (as the launcher's "Full screen"), settings kind, most cabinets
$Games = @(
    @{ Name = 'Tokyo Wars';             Exe = 'TokyoWars-ArcadeLAN.exe';       Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Rave Racer';             Exe = 'RaveRacer-ArcadeLAN.exe';       Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Ace Driver';             Exe = 'AceDriver-ArcadeLAN.exe';       Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Ace Driver Victory Lap'; Exe = 'VictoryLap-ArcadeLAN.exe';      Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Cyber Commando';         Exe = 'CyberCommando-ArcadeLAN.exe';   Args = 'roms --fullscreen';       Kind = 'link'; Max = 2 },
    @{ Name = 'Cyber Cycles';           Exe = 'CyberCycles-ArcadeLAN.exe';     Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Armadillo Racing';       Exe = 'ArmadilloRacing-ArcadeLAN.exe'; Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Alpine Racer 2';         Exe = 'AlpineRacer2-ArcadeLAN.exe';    Args = 'roms --fullscreen';       Kind = 'link'; Max = 4 },
    @{ Name = 'Cyber Sled';             Exe = 'CyberSled-ArcadeLAN.exe';       Args = 'extracted --fullscreen';  Kind = 'link'; Max = 2 },
    @{ Name = 'Gunmen Wars';            Exe = 'GunmenWars-ArcadeLAN.exe';      Args = '--window --fullscreen';   Kind = 'gw';   Max = 4 },
    @{ Name = 'Time Crisis II';         Exe = 'TimeCrisis2-ArcadeLAN.exe';     Args = '--window --fullscreen';   Kind = 'tc2';  Max = 2 }
)

function Read-Saved {
    $s = @{}
    if (Test-Path $CfgFile) { foreach ($l in Get-Content $CfgFile) { if ($l -match '^\s*([^=#]+?)\s*=\s*(.*)$') { $s[$Matches[1]] = $Matches[2] } } }
    return $s
}
function Save-Saved($s) { ($s.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) | Set-Content -Encoding ASCII $CfgFile }

# one key of a key=value settings file set, every other line kept as it is
function Set-Key($path, $key, $value) {
    $lines = @(); if (Test-Path $path) { $lines = @(Get-Content $path) }
    $done = $false
    $out = foreach ($l in $lines) {
        if ($l -notmatch '^\s*#' -and $l -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { if (-not $done) { $done = $true; "$key=$value" } }
        else { $l }
    }
    if (-not $done) { $out = @($out) + "$key=$value" }
    $out | Set-Content -Encoding ASCII $path
}

function Ask($prompt, $default) {
    $a = Read-Host ("$prompt" + $(if ($default) { " [$default]" } else { '' }))
    if ([string]::IsNullOrWhiteSpace($a)) { return $default } else { return $a.Trim() }
}

# ------------------------------------------------------------------------------------------------------------------------------
if ($Share) {
    New-Item -ItemType Directory -Force $LocalResults | Out-Null
    & icacls $LocalResults /grant '*S-1-1-0:(OI)(CI)M' | Out-Null          # Everyone may write (S-1-1-0 = Everyone, in any language)
    # net's messages go through cmd: in Windows PowerShell 5.1 any text a program writes to its error output stops this script
    & cmd /c "net share $ShareName /delete /y >nul 2>nul"                  # an old share of the same name (usually none)
    & cmd /c "net share ""$ShareName=$LocalResults"" /GRANT:*S-1-1-0,FULL >nul 2>nul"
    if ($LASTEXITCODE -ne 0) { & cmd /c "net share ""$ShareName=$LocalResults"" /GRANT:Everyone,FULL >nul 2>nul" }
    if ($LASTEXITCODE -ne 0) { Write-Host "Could not share the folder (run this as administrator)." -ForegroundColor Red; exit 1 }
    $s = Read-Saved; $s['results'] = $LocalResults; Save-Saved $s
    Write-Host ""
    Write-Host "The results folder is shared." -ForegroundColor Green
    Write-Host "  On this PC:     $LocalResults"
    Write-Host "  On the others:  \\$env:COMPUTERNAME\$ShareName" -ForegroundColor Yellow
    $ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notmatch '^(127|169\.254)\.' } | ForEach-Object { $_.IPAddress })
    foreach ($ip in $ips) { Write-Host "             or:  \\$ip\$ShareName" }
    Write-Host ""
    Write-Host "Type that into ""2 - Every PC - run a test.bat"" on the other PCs when it asks for the results folder."
    exit 0
}

# ------------------------------------------------------------------------------------------------------------------------------
if ($Run) {
    $s = Read-Saved
    Write-Host ""
    Write-Host "NAMCO 22 ARCADE LAN - LAN TEST" -ForegroundColor Cyan
    Write-Host "Run this on every PC of the test. Each PC needs its own cabinet number (1, 2, 3 ...)."
    Write-Host ""
    for ($i = 0; $i -lt $Games.Count; $i++) { Write-Host ("  {0,2}. {1}" -f ($i + 1), $Games[$i].Name) }
    Write-Host ""
    $def = $s['game']; $gi = -1
    while ($gi -lt 0 -or $gi -ge $Games.Count) { $a = Ask 'Game (number)' $def; $n = 0; if ([int]::TryParse("$a", [ref]$n)) { $gi = $n - 1 } }
    $G = $Games[$gi]
    $nc = 0
    while ($nc -lt 2 -or $nc -gt $G.Max) { $a = Ask "How many cabinets in the test (2-$($G.Max))" $(if ($s['cabinets']) { $s['cabinets'] } else { '2' }); [void][int]::TryParse("$a", [ref]$nc) }
    $cab = 0
    $hint = if ($G.Kind -eq 'tc2') { ' (1 = left / red, 2 = right / blue)' } else { '' }
    while ($cab -lt 1 -or $cab -gt $nc) { $a = Ask "This PC's cabinet number (1-$nc)$hint" $s['cabinet']; [void][int]::TryParse("$a", [ref]$cab) }
    $res = Ask 'Results folder (the main PC''s shared folder, e.g. \\MAINPC\N22LANTEST)' $(if ($s['results']) { $s['results'] } else { $LocalResults })
    $s['game'] = $gi + 1; $s['cabinets'] = $nc; $s['cabinet'] = $cab; $s['results'] = $res; Save-Saved $s

    # the checks that stop most real tests: a "Public" network (Windows' firewall then blocks the game), the results folder
    $prof = @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)
    foreach ($p in $prof) { if ("$($p.NetworkCategory)" -eq 'Public') {
        Write-Host ""
        Write-Host "WARNING: the network ""$($p.Name)"" is set to PUBLIC on this PC: Windows may block the link." -ForegroundColor Yellow
        Write-Host "         Settings > Network & internet > (your connection) > Private network." -ForegroundColor Yellow } }
    $resOk = $true
    try { New-Item -ItemType Directory -Force $res | Out-Null; $t = Join-Path $res ".write-test-$env:COMPUTERNAME"; 'x' | Set-Content $t; Remove-Item $t } catch { $resOk = $false }
    if (-not $resOk) {
        Write-Host ""
        Write-Host "Cannot write to $res - the results will be kept on this PC instead ($LocalResults)." -ForegroundColor Yellow
        Write-Host "(Windows may need the main PC's user name and password first: open $res in File Explorer once.)" -ForegroundColor Yellow
        $res = $LocalResults; New-Item -ItemType Directory -Force $res | Out-Null
    }

    $dir = Join-Path $Kit ("games\" + $G.Name)
    if (-not (Test-Path (Join-Path $dir $G.Exe))) { Write-Host "Not found: $dir\$($G.Exe)" -ForegroundColor Red; exit 1 }
    $cfgName = switch ($G.Kind) { 'link' { 'link.cfg' } 'gw' { 'gunwars.cfg' } 'tc2' { 'tc2_link.cfg' } }
    $cfg = Join-Path $dir $cfgName
    $backup = "$cfg.before-lan-test"
    if (Test-Path $cfg) { Copy-Item -Force $cfg $backup } else { Remove-Item -Force -ErrorAction SilentlyContinue $backup }

    # THE TEST'S LINK SETTINGS: on, this number, Automatic finding, a minute's wait, recordings on (a problem can be replayed)
    switch ($G.Kind) {
        'link' { Set-Key $cfg 'link' 'on'; Set-Key $cfg 'cabinet' $cab; Set-Key $cfg 'cabinets' $nc; Set-Key $cfg 'find' 'auto'
                 Set-Key $cfg 'wait' '60'; Set-Key $cfg 'record' 'on'; Set-Key $cfg 'method' 'arcade'; Set-Key $cfg 'set_game_id' '1' }
        'gw'   { Set-Key $cfg 'link' 'on'; Set-Key $cfg 'link_cabinet' $cab; Set-Key $cfg 'link_cabinets' $nc; Set-Key $cfg 'link_address' 'auto'
                 Set-Key $cfg 'link_wait' '60' }
        'tc2'  { Set-Key $cfg 'link' 'on'; Set-Key $cfg 'side' $(if ($cab -eq 1) { 'left' } else { 'right' }); Set-Key $cfg 'address' 'auto'
                 Set-Key $cfg 'wait' '60'; Set-Key $cfg 'record' 'on' }
    }

    $start = Get-Date
    Write-Host ""
    Write-Host "Starting $($G.Name) as cabinet $cab of $nc. Start it on the other PCs within a minute." -ForegroundColor Green
    Write-Host "Play, then close the game (Esc > Exit). Its logs then go to the results folder."
    Write-Host "Something looks wrong? Press F9 in the game first: it saves a capture that goes with the logs."
    $env:TC2_SIDE = $null; $env:S23_SIDE = $null
    $p = Start-Process -FilePath (Join-Path $dir $G.Exe) -ArgumentList $G.Args -WorkingDirectory $dir -PassThru
    $p.WaitForExit()
    $end = Get-Date

    # THE RESULTS: <results>\<date>\<game>\<PC> cabinet N <time>\  the logs, recordings, settings and captures of this run, and pc-info.txt
    $out = Join-Path $res ("{0}\{1}\{2} cabinet {3} {4}" -f $start.ToString('yyyy-MM-dd'), $G.Name, $env:COMPUTERNAME, $cab, $start.ToString('HH-mm-ss'))
    New-Item -ItemType Directory -Force $out | Out-Null
    Get-ChildItem $dir -File | Where-Object { $_.Extension -in '.log', '.rec', '.cfg' -or $_.Name -like '*.nv' } |
        Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-5) -or $_.Name -eq $cfgName } | Copy-Item -Destination $out -Force
    if (Test-Path (Join-Path $dir 'captures')) {
        $caps = @(Get-ChildItem (Join-Path $dir 'captures') -File -Recurse | Where-Object { $_.LastWriteTime -ge $start })
        if ($caps.Count) { $cd = Join-Path $out 'captures'; New-Item -ItemType Directory -Force $cd | Out-Null; $caps | Copy-Item -Destination $cd -Force }
    }
    $info = @()
    $info += "PC:          $env:COMPUTERNAME"
    $info += "Game:        $($G.Name)  (cabinet $cab of $nc)"
    $info += "Started:     $($start.ToString('yyyy-MM-dd HH:mm:ss'))"
    $info += "Closed:      $($end.ToString('yyyy-MM-dd HH:mm:ss'))   exit code $($p.ExitCode)"
    $info += "Addresses:   " + ((Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notmatch '^127\.' } | ForEach-Object { "$($_.IPAddress) ($($_.InterfaceAlias))" }) -join ', ')
    $info += "Networks:    " + (($prof | ForEach-Object { "$($_.Name) = $($_.NetworkCategory) via $($_.InterfaceAlias)" }) -join '; ')
    $info += "Windows:     " + [Environment]::OSVersion.VersionString
    $info | Set-Content -Encoding UTF8 (Join-Path $out 'pc-info.txt')

    # the game's own settings back as they were
    if (Test-Path $backup) { Copy-Item -Force $backup $cfg; Remove-Item -Force $backup }
    Write-Host ""
    Write-Host "Done. The logs are in:" -ForegroundColor Green
    Write-Host "  $out"
    Write-Host "$($G.Name)'s own link settings are back as they were."
    exit 0
}

# ------------------------------------------------------------------------------------------------------------------------------
if ($Check) {
    $s = Read-Saved
    $res = if ($s['results'] -and (Test-Path $s['results'])) { $s['results'] } else { $LocalResults }
    if (-not (Test-Path $res)) { Write-Host "No results yet ($res)."; exit 0 }
    Write-Host "Results in $res" -ForegroundColor Cyan
    foreach ($day in Get-ChildItem $res -Directory | Sort-Object Name) {
        foreach ($game in Get-ChildItem $day.FullName -Directory | Sort-Object Name) {
            Write-Host ""
            Write-Host "$($day.Name)  $($game.Name)" -ForegroundColor Cyan
            foreach ($one in Get-ChildItem $game.FullName -Directory | Sort-Object Name) {
                $logs = @(Get-ChildItem $one.FullName -Filter *.log -File)
                $text = if ($logs.Count) { ($logs | ForEach-Object { Get-Content $_.FullName -ErrorAction SilentlyContinue }) } else { @() }
                $linked = @($text | Select-String -SimpleMatch -CaseSensitive -Pattern '[LINK] linked', '[LINK] LINKED:', '[C422] LINKED' | Select-Object -First 1)
                $alone  = @($text | Select-String -Pattern 'NOT linked', 'plays alone', 'playing solo', 'No other cabinets found' | Select-Object -First 1)
                $waiting = @($text | Select-String -SimpleMatch -Pattern 'Waiting for', 'WAITING FOR' | Select-Object -First 1)

                # "linked" alone is not enough: the game's own lines say whether it really worked (red = it did not, yellow = worth a look)
                $bad = New-Object System.Collections.Generic.List[string]; $note = New-Object System.Collections.Generic.List[string]
                $who = @($text | Select-String -CaseSensitive -Pattern '^\[LINK\] frame \d+  (Hears|Hears nobody)')          # who it hears, every 5 s
                $oneway = @($who | Where-Object { $_.Line -match 'Not heard by' })
                if ($who.Count -ge 2 -and $oneway.Count * 2 -ge $who.Count) {
                    $by = [regex]::Match($oneway[-1].Line, 'Not heard by ([\d, ]+)').Groups[1].Value.Trim()
                    $bad.Add("ONE-WAY: cabinet(s) $by could not hear this PC ($($oneway.Count) of $($who.Count) checks)") }
                $game = @($text | Select-String -CaseSensitive -Pattern '^\[LINK\] frame \d+  down .*\| game:')            # the game's own view
                $down = @($game | Where-Object { $_.Line -match 'link down' })
                if ($game.Count -ge 2 -and $down.Count * 2 -ge $game.Count) { $bad.Add("The game's link stayed DOWN ($($down.Count) of $($game.Count) checks)") }
                if ($text | Select-String -SimpleMatch -Pattern 'on the network at 0.0.0.0', '(0.0.0.0)' | Select-Object -First 1) { $bad.Add('A cabinet was found at 0.0.0.0: copy the latest package to every PC') }
                $ranout = @($text | Select-String -SimpleMatch -Pattern '(the wait ran out)' | Select-Object -First 1)
                if ($ranout.Count) { $note.Add('Started without every cabinet (the wait ran out): ' + (($ranout[0].Line -replace '.*\): ', '') -replace ', session.*', '')) }
                $lostl = @($text | Select-String -SimpleMatch -Pattern 'lost the link', 'LINK LOST')
                if ($lostl.Count) { $note.Add("The game lost the link $($lostl.Count) time(s), first: " + ($lostl[0].Line -replace '^\[\w+\] ', '')) }
                foreach ($w in @($text | Select-String -Pattern 'WARNING', 'DIFFERS', 'has not answered' | Select-Object -First 3)) { $note.Add(($w.Line -replace '^\s+', '')) }
                $q = @($text | Select-String -CaseSensitive -Pattern 'quality: .*(POOR|fair)')
                if ($q.Count) { $note.Add("Link quality not 'good' in $($q.Count) check(s), e.g. " + ($q[0].Line -replace '.*quality: ', '')) }
                $netlost = 0; foreach ($l in $game) { $m = [regex]::Match($l.Line, 'net sent \d+ recv \d+ lost (\d+)'); if ($m.Success) { $netlost = [Math]::Max($netlost, [int]$m.Groups[1].Value) } }
                foreach ($l in @($text | Select-String -Pattern '(\d+) datagrams lost')) { $netlost = [Math]::Max($netlost, [int][regex]::Match($l.Line, '(\d+) datagrams lost').Groups[1].Value) }
                if ($netlost -gt 0) { $note.Add("$netlost message(s) lost on the network") }
                $pace = @($text | Select-String -Pattern '(\d+) over 20 ms') ; $slow = @($pace | Where-Object { [int][regex]::Match($_.Line, '(\d+) over 20 ms').Groups[1].Value -ge 30 })
                if ($slow.Count) { $note.Add("This PC ran below full speed in $($slow.Count) of $($pace.Count) 10-second stretches (the PC, not the network)") }

                $state = if ($linked.Count -and $bad.Count) { 'LINKED, BUT NOT WORKING' } elseif ($linked.Count) { 'LINKED' } elseif ($alone.Count) { 'NOT LINKED' } elseif ($waiting.Count) { 'CLOSED WHILE WAITING FOR THE OTHERS' } else { 'NO LINK LINES IN THE LOG' }
                $col = if ($state -eq 'LINKED' -and -not $note.Count) { 'Green' } elseif ($state -eq 'LINKED') { 'Yellow' } else { 'Red' }
                Write-Host ("  {0,-40} {1}" -f $one.Name, $state) -ForegroundColor $col
                if ($linked.Count) { Write-Host ("      " + ($linked[0].Line -replace '^\s+', '')) }
                if ($alone.Count -and -not $linked.Count) { Write-Host ("      " + ($alone[0].Line -replace '^\s+', '')) }
                foreach ($b in $bad) { Write-Host ("      " + $b) -ForegroundColor Red }
                foreach ($w in $note) { Write-Host ("      " + $w) -ForegroundColor Yellow }
            }
        }
    }
    exit 0
}
Write-Host "Use one of the .bat files beside this script."
