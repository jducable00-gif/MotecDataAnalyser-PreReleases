# =============================================================================
#  release.ps1 -- Prepare and publish a new MotecDataAnalyser version tag.
#
#  Usage (from the repo root):
#      .\scripts\release.ps1 v5.7        # bump kAppVersion, commit, tag, push
#      .\scripts\release.ps1 5.8         # leading "v" is optional
#
#  What it does:
#    1. Requires a clean git working tree (commit your work first).
#    2. Bumps kAppVersion in ReleaseInfo.h to the new version ("VX.Y").
#    3. Commits that single file with message "Release vX.Y".
#    4. Creates an annotated tag vX.Y and pushes branch + tag to origin.
#
#  After it finishes, create/publish the GitHub release (with its notes) from
#  the web UI -- a link is printed at the end of the run. The app picks up the
#  new notes automatically on next launch via api.github.com/releases/latest.
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string]$Version,  # ex : v5.7 ou 5.8
    [switch]$Force                                    # skip the interactive "Proceed?" prompt (non-interactive hosts)

)

$ErrorActionPreference = 'Stop'

# --- Resolve paths -----------------------------------------------------------
$ScriptDir  = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not (Test-Path $ScriptDir)) { throw "Cannot determine the scripts directory." }
$ProjectDir = Split-Path -Parent $ScriptDir   # this script lives in <root>\scripts\

# --- Normalize version -------------------------------------------------------
$vRaw  = $Version.Trim()
if ($vRaw -match '^[Vv]') { $vRaw = $vRaw.Substring(1) }    # strip optional leading v/V
if (-not ($vRaw -match '^\d+\.\d+$')) {
    throw "Invalid version '$Version'. Expected e.g. 5.7 or v5.7."
}
$tag     = "v$vRaw"      # git tag, ex : v5.7
$appVer  = "V$vRaw"      # kAppVersion literal (uppercase V), ex : "V5.7"

Push-Location $ProjectDir
try {
    & git rev-parse --is-inside-work-tree *> $null
    if ($LASTEXITCODE -ne 0) { throw "'$ProjectDir' is not a git repository." }

    # --- Pre-flight checks ---------------------------------------------------
    $dirty = (& git status --porcelain) | Where-Object { $_.Trim() }
    if ($dirty.Count -gt 0) {
        Write-Warning "Working tree has uncommitted changes:"
        $dirty | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
        throw "Commit (or stash) your work first, then re-run .\scripts\release.ps1 $tag."
    }

    # NOTE: on a missing tag git writes to stderr, which PowerShell surfaces as a
    # NativeCommandError (even with `*> $null` in some hosts). Silence EAP around the
    # probe and judge purely by exit code.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    & git rev-parse --verify ("refs/tags/" + $tag) | Out-Null
    $tagExists = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $prevEap
    if ($tagExists) { throw "Tag '$tag' already exists. Pick a new version number." }

    $relInfo = Join-Path $ProjectDir 'ReleaseInfo.h'
    if (-not (Test-Path $relInfo)) { throw "ReleaseInfo.h not found: $relInfo" }

    # Current kAppVersion, for the confirmation summary.
    $curLine  = Select-String -Path $relInfo -Pattern 'kAppVersion\s*=\s*"([^"]+)"' | Select-Object -First 1
    if (-not $curLine) { throw "Could not locate kAppVersion in ReleaseInfo.h." }
    $currentVer = [regex]::Match($curLine.Line, '"[^"]+"').Value.Trim('"')

    Write-Host ""
    Write-Host "Release plan:" -ForegroundColor Cyan
    Write-Host ("  current : {0}" -f $currentVer)
    Write-Host ("  new     : {0}   (tag: {1})" -f $appVer, $tag)

    if ($currentVer.ToLower() -eq $tag) {
        throw "kAppVersion is already '$currentVer' -- nothing to release."
    }

    $proceed = [bool]$Force
    if (-not $proceed) {
        $answer = Read-Host "Proceed? (yes/no)"
        $proceed = ($answer.Trim().ToLower() -in @('y', 'yes'))
    }
    if (-not $proceed) {
        Write-Host "Aborted. No changes made." -ForegroundColor Yellow
        return
    }

    # --- Bump kAppVersion ----------------------------------------------------
    $bytes  = [System.IO.File]::ReadAllBytes($relInfo)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)

    $text   = [System.IO.File]::ReadAllText($relInfo, (New-Object System.Text.UTF8Encoding $true))
    $newTxt = [regex]::Replace($text, '(kAppVersion\s*=\s*)"[^"]+"', '${1}"' + $appVer + '"')
    if ($newTxt -eq $text) { throw "Failed to update kAppVersion in ReleaseInfo.h." }

    # Write back with the same BOM state as before.
    [System.IO.File]::WriteAllText($relInfo, $newTxt, (New-Object System.Text.UTF8Encoding $hasBom))
    Write-Host "==> kAppVersion -> '$appVer' in ReleaseInfo.h" -ForegroundColor Green

    # --- Commit + tag --------------------------------------------------------
    & git add ReleaseInfo.h | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git add failed." }

    $msg = "Release $tag"
    & git commit -m $msg | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git commit failed." }
    Write-Host ("==> Committed: {0}" -f $msg) -ForegroundColor Green

    & git tag -a $tag -m $msg | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git tag failed." }
    Write-Host ("==> Tagged: {0} (annotated)" -f $tag) -ForegroundColor Green

    # --- Push ----------------------------------------------------------------
    & git push origin HEAD
    if ($LASTEXITCODE -ne 0) { throw "Push of branch to origin failed. The tag exists locally; re-run the push manually." }

    & git push origin $tag | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning ("Branch pushed, but pushing tag '{0}' failed -- run: git push origin {1}" -f $tag, $tag); return }

    # --- Next step (manual): create the GitHub release -----------------------
    $branch = (& git rev-parse --abbrev-ref HEAD).Trim()
    Write-Host ""
    Write-Host "Done. Last manual step: publish the release notes on GitHub." -ForegroundColor Cyan
    Write-Host ("  https://github.com/jducable00-gif/MotecDataAnalyser/releases/new?target={0}&tag={1}" -f $branch, $tag)
}
finally {
    Pop-Location
}

