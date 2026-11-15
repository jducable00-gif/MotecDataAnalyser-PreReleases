# =============================================================================
# run_tests.ps1 - one-command build + run for all standalone test harnesses.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run_tests.ps1          # everything (default)
#   ... -Only pmr_decoder,tablet_server                                     # subset by short name
#   ... -SkipBuild                                                          # reuse existing .exe files
#
# Harnesses:
#   pmr_decoder         self-contained decoder checks
#   pmr_feed            UDP loopback feed test (binds 127.0.0.1)
#   pmr_socket          socket pump smoke test
#   report_json         needs a Motec .ld/.ldx session file as input; SKIPPED
#                       with a clear message when no sample is found under
#                       data\samples\, samples\ or the repo root (data/ is gitignored)
#   tablet_server       binds 127.0.0.1:18765 briefly, stops itself
#   comparison_engine   diagnostic engine + legacy rule non-regression suite
#
# Pure ASCII on purpose (PowerShell 5.1 reads BOM-less .ps1 as cp1252).
# Exit code: 0 = all built and passed; 1 = at least one build/test failure.
# Per-test console output is kept in scripts\<name>.log for inspection.
# =============================================================================

param(
    [string[]]$Only,      # short names to run (default: all)
    [switch]$SkipBuild,   # do not rebuild; use existing .exe files
    [int]$TimeoutSec = 180        # per-TEST timeout (seconds)
)

# Multi-TU bcc32c builds can take several minutes; keep this well above $TimeoutSec.
$buildTimeoutSec = if ($TimeoutSec -ge 600) { $TimeoutSec } else { 900 }

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir  = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$repoRoot   = (Resolve-Path (Join-Path $scriptDir "..")).Path
$rsvars     = "C:\Program Files (x86)\Embarcadero\Studio\23.0\bin\rsvars.bat"

# Temp dir for the generated .bat wrappers; cleaned up before every exit.
$tempBatDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mda_tests_" + [guid]::NewGuid().ToString("N").Substring(0,8))
New-Item -ItemType Directory -Force -Path $tempBatDir | Out-Null

# --- harness table: short name, exe, bcc32c args (relative to scripts\) ------
$HARNESS = @(
    # LiveChannelResolver.cpp: PmrLiveCapture.h declares ResolvedChannelTable (defined there).
    # RuleConfig.cpp: LiveChannelResolver.o references RuleConfig::instance().
    @{ Name="pmr_decoder";       Exe="test_pmr_decoder.exe";
        Args=@("-I..","-o","test_pmr_decoder.exe", "..\PmrLiveCapture.cpp", "..\LiveChannelResolver.cpp", "..\RuleConfig.cpp", "test_pmr_decoder.cpp") },
    @{ Name="pmr_feed";          Exe="test_pmr_feed.exe";
        Args=@("-I..","-o","test_pmr_feed.exe", "..\PmrLiveCapture.cpp", "..\LiveChannelResolver.cpp", "..\RuleConfig.cpp", "test_pmr_feed.cpp") },
    @{ Name="pmr_socket";        Exe="test_pmr_socket.exe";
        Args=@("-I..","-o","test_pmr_socket.exe", "..\PmrLiveCapture.cpp", "..\LiveChannelResolver.cpp", "..\RuleConfig.cpp", "test_pmr_socket.cpp") },
    # VehicleDb*.cpp + ReleaseInfo.cpp: LapReportGenerator now references Ams2VehicleDb (same set as comparison_engine).
    @{ Name="report_json";       Exe="test_report_json.exe"; NeedsDataFile=$true;
        Args=@("-I..","-o","test_report_json.exe", "..\analyze_motec_setup.cpp", "..\MotecToCSV.cpp",
              "..\RuleConfig.cpp", "..\LapReportGenerator.cpp", "..\VehicleDb.cpp", "..\ReleaseInfo.cpp",
              "..\VehicleDbData_A_F.cpp","..\VehicleDbData_F_M.cpp","..\VehicleDbData_M_S.cpp","..\VehicleDbData_S_V.cpp",
              "test_report_json.cpp", "rtl.lib","vcl.lib","cp32mt.lib") },
    # No vcl/cp32mt: HttpServer + test are pure C++; cp32mt's VCL-aware CRT drags in unsatisfiable System:: symbols.
    # Explicit libs (bare names, resolved via rsvars env): psdk import set for WinAPI refs from the objects,
    # win32c cp32mti.lib for std::thread/mutex/atomic (__Mtx_*/__Cnd_*), rtle.lib last for __InitVCL/__ExitVCL. Order matters (TIL). Sockets via pragma comment in test file.
    @{ Name="tablet_server";     Exe="test_tablet_server.exe";
        Args=@("-I..","-o","test_tablet_server.exe", "..\HttpServer.cpp", "test_tablet_server.cpp",
              "rtl.lib","kernel32.lib","user32.lib","gdi32.lib","advapi32.lib","shcore.lib","wtsapi32.lib","Uxtheme.lib","windowscodecs.lib",
              "cp32mti.lib","rtle.lib") },
    @{ Name="comparison_engine"; Exe="test_comparison_engine.exe";
        Args=@("-I..","-o","test_comparison_engine.exe", "..\analyze_motec_setup.cpp", "..\MotecToCSV.cpp",
              "..\RuleConfig.cpp", "..\LapReportGenerator.cpp", "..\VehicleDb.cpp", "..\ReleaseInfo.cpp",
              "..\VehicleDbData_A_F.cpp","..\VehicleDbData_F_M.cpp","..\VehicleDbData_M_S.cpp","..\VehicleDbData_S_V.cpp",
              "test_comparison_engine.cpp", "rtl.lib","vcl.lib","cp32mt.lib","winhttp.lib") }
)

# --- helpers ------------------------------------------------------------------
function Remove-TempBatDir() {
    if (Test-Path $tempBatDir) { Remove-Item -Recurse -Force $tempBatDir -ErrorAction SilentlyContinue }
}

function Test-WaitForExit([System.Diagnostics.Process]$p, [int]$TimeoutSec) {
    # Bounded wait: returns true if the process exited within TimeoutSec.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $p.HasExited -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 100 }
    return $p.HasExited
}

function Stop-ProcessTree([int]$Pid_) {
    # Kill children first (cmd.exe -> bcc32c/exe), then the root. Best effort only:
    # a timeout means we are giving up on this test, so never throw here.
    try {
        Get-CimInstance Win32_Process -Filter ("ParentId = " + $Pid_) | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    } catch {}
    try { Stop-Process -Id $Pid_ -Force -ErrorAction SilentlyContinue } catch {}
}

function Write-BatFile([string]$Path, [string[]]$Lines) {
    # ASCII-only content; cmd.exe reads .bat as ANSI.
    Set-Content -LiteralPath $Path -Value ($Lines | ForEach-Object { $_ + "`r`n" }) `
                -Encoding Ascii -NoNewline
}

function Run-BatBounded([string]$Bat, [string]$LogPath, [int]$TimeoutSec) {
    # Runs a .bat file; the bat itself redirects its output to $LogPath (see Test-*Harness).
    foreach ($f in @($LogPath, $LogPath + ".err")) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
    try {
        # Redirection lives INSIDE the .bat file. Do NOT pass ">" / "2>" through Start-Process's
        # ArgumentList: PS quotes any argument containing a special character, so cmd.exe receives
        # literal '>C:\...' text instead of redirection (observed: empty logs + bogus exit codes).
        $p = Start-Process -FilePath "cmd.exe" `
               -ArgumentList @("/c", '"' + $Bat + '"') `
               -WorkingDirectory (Split-Path $Bat) -NoNewWindow -PassThru
    } catch {
        Add-Content -LiteralPath $LogPath -Value ("ERROR: " + $_.Exception.Message)
        return 1200
    }
    if (-not (Test-WaitForExit $p $TimeoutSec)) { Stop-ProcessTree $p.Id; return "timeout" }
    Start-Sleep -Milliseconds 50   # let redirected stdout flush before we read it
    try {
        return [int]$p.ExitCode    # process exit code, or the string "timeout"
    } catch {
        Add-Content -LiteralPath $LogPath -Value ("ERROR: reading ExitCode failed: " + $_.Exception.Message)
        return 1201
    }
}

function Test-BuildHarness {
    param([hashtable]$H, [string]$LogPath)
    # rsvars.bat + bcc32c must run in ONE cmd.exe process (env does not persist).
    # Redirection is inside the bat (not via Start-Process args): bcc32c's exit code becomes cmd's.
    $bat = Join-Path $tempBatDir ("build_" + $H.Name + ".tmp.bat")
    Write-BatFile -Path $bat -Lines @(
        '@echo off',
        ('call "' + $rsvars + '" >nul 2>&1'),
        ('cd /d "' + $scriptDir + '"'),
        ('del /q ' + $H.Exe),
        ('bcc32c ' + ($H.Args -join " ") + ' >"' + $LogPath + '" 2>"' + $LogPath + '.err"')
    )

    # Builds get their own (longer) timeout: a zero/short value here used to kill the build
    # instantly and orphan bcc32c, which then kept writing logs in the background.
    return (Run-BatBounded -Bat $bat -LogPath $LogPath -TimeoutSec $buildTimeoutSec)
}

function Test-RunHarness {
    param([hashtable]$H, [string[]]$ExtraArgs, [string]$LogPath, [int]$TimeoutSec = 180)
    if (-not (Test-Path (Join-Path $scriptDir $H.Exe))) { return "missing" }
    # bcc32c console apps are ANSI; a UTF-16 log would garble their output.
    try { [System.IO.File]::Delete((Join-Path $scriptDir $LogPath)) } catch {}
    # Redirection is inside the bat (not via Start-Process args): the exe's exit code becomes cmd's.
    $exePath = Join-Path $scriptDir $H.Exe
    $bat     = Join-Path $tempBatDir ("run_" + $H.Name + ".tmp.bat")
    Write-BatFile -Path $bat -Lines @(
        '@echo off',
        'chcp 65001 >nul',
        ('cd /d "' + $scriptDir + '"')
    )
    if ($ExtraArgs) {
        $exeLine = '"' + $exePath + '" ' + (($ExtraArgs | ForEach-Object { '"' + $_ + '"' }) -join " ")
    } else {
        $exeLine = '"' + $exePath + '"'
    }
    Add-Content -LiteralPath $bat -Value ($exeLine + ' >"' + $LogPath + '" 2>"' + $LogPath + '.err"') `
                -Encoding Ascii -NoNewline
    return (Run-BatBounded -Bat $bat -LogPath $LogPath -TimeoutSec $TimeoutSec)
}

# --- select harnesses ---------------------------------------------------------
$selected = if ($Only) { @($HARNESS | Where-Object { $Only -contains $_.Name }) } else { $HARNESS }
if (-not $selected.Count) { Write-Host "No matching harness in: $($Only -join ', ')" ; Remove-TempBatDir; exit 1 }

# --- locate a sample .ld/.ldx for report_json ----------------------------------
$sampleFile = $null
foreach ($dir in @((Join-Path $repoRoot "data\samples"), (Join-Path $repoRoot "samples"))) {
    if (Test-Path $dir) {
        # Recurse into subdirs (AMS2/, AC_EVO/); use .Extension to avoid PS5.1 -Include quirks.
        $f = Get-ChildItem -LiteralPath $dir -Recurse -File |
             Where-Object { $_.Extension -eq ".ld" -or $_.Extension -eq ".ldx" } | Select-Object -First 1
        if ($f) { $sampleFile = $f.FullName; break }
    }
}
if (-not $sampleFile) {
    # Fallback: repo root, proper extension filter (avoids matching .gitignore etc.)
    $f = Get-ChildItem -LiteralPath $repoRoot -Recurse -Depth 4 -File |
         Where-Object { ($_.Extension -eq ".ld" -or $_.Extension -eq ".ldx") } |
         Where-Object { $_.FullName -notmatch "\\(Win32|scripts)\\" } | Select-Object -First 1
    if ($f) { $sampleFile = $f.FullName }
}

# --- build --------------------------------------------------------------------
Write-Host ""
$builtOk = @{}   # name -> true for harnesses built successfully in THIS run; a stale .exe from an
                 # earlier session must never mask a failed build (false-positive guard)
if (-not $SkipBuild) {
    Write-Host "=== Building $($selected.Count) harness(es) (bcc32c via rsvars.bat) ===" -ForegroundColor Cyan
    foreach ($h in $selected) {
        $log = Join-Path $scriptDir ("build_" + $h.Name + ".log")
        Write-Host ("  build " + $h.Name.PadRight(18)) -NoNewline
        if ((Test-BuildHarness -H $h -LogPath $log) -eq 0) {   # exit code; 0 = success (PS: bare 0 is falsy!)
            Write-Host " OK" -ForegroundColor Green
            $builtOk[$h.Name] = $true
        } else {
            Write-Host (" FAILED -> scripts\build_" + $h.Name + ".log") -ForegroundColor Red
        }
    }
}

# --- run ----------------------------------------------------------------------
Write-Host ""
$passCount = 0; $failList = @(); $skipList = @()
foreach ($h in $selected) {
    if (-not (Test-Path (Join-Path $scriptDir $h.Exe))) {
        Write-Host ("  " + $h.Name.PadRight(18)) -NoNewline
        Write-Host " SKIPPED (build failed or --SkipBuild with no .exe)" -ForegroundColor Yellow
        if ($SkipBuild) { $skipList += $h.Name } else { $failList += $h.Name }
        continue
    }

    # A build that just FAILED must not be run against a stale exe from an earlier session.
    if (-not $SkipBuild -and -not $builtOk.ContainsKey($h.Name)) {
        Write-Host ("  " + $h.Name.PadRight(18)) -NoNewline
        Write-Host " SKIPPED (build failed; refusing to run stale .exe)" -ForegroundColor Yellow
        if ($failList -notcontains $h.Name) { $failList += $h.Name }   # already listed by the build loop
        continue
    }

    # report_json needs a session file; skip cleanly when none is available.
    if (($h.ContainsKey("NeedsDataFile") -and $h.NeedsDataFile) -and -not $sampleFile) {
        Write-Host ("  " + $h.Name.PadRight(18)) -NoNewline
        Write-Host " SKIPPED (no .ld/.ldx sample under data\samples\, samples\ or repo root)" -ForegroundColor Yellow
        $skipList += $h.Name
        continue
    }

    # Only report_json takes the sample .ld/.ldx (argv[1]); other harnesses are self-contained,
    # and tablet_server treats any argument as a usage error.
    $extra = @()
    if ($h.Name -eq "report_json" -and $sampleFile) { $extra = @($sampleFile) }
    Write-Host ("  run   " + $h.Name.PadRight(18)) -NoNewline
    $rc = Test-RunHarness -H $h -ExtraArgs $extra `
                          -LogPath (Join-Path $scriptDir ($h.Name + ".log")) -TimeoutSec $TimeoutSec
    switch ($rc) {
        0       { Write-Host " PASS" -ForegroundColor Green;   $passCount++ }
        "timeout"{ Write-Host (" TIMEOUT after " + $TimeoutSec + "s") -ForegroundColor Red; $failList += $h.Name }
        default { Write-Host (" FAILED (exit " + $rc + ") -> scripts\" + $h.Name + ".log" ) -ForegroundColor Red; $failList += $h.Name }
    }
}

# --- summary ------------------------------------------------------------------
Write-Host ""
$skipNote = if ($skipList) { "; skipped: $($skipList -join ', ')" } else { "" }
if (-not $SkipBuild) { Write-Host ("Built " + $selected.Count + " harness(es).") }
if ($failList) {
    Write-Host ("RESULT: FAIL -- failed: $($failList -join ', ')") -ForegroundColor Red
    Remove-TempBatDir; exit 1
} else {
    Write-Host ("RESULT: PASS -- " + $passCount + "/" + $selected.Count + " passed" + $skipNote) -ForegroundColor Green
    Remove-TempBatDir; exit 0
}
