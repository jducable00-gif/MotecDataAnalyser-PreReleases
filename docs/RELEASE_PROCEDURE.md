# Release Procedure (agent-facing)

How to cut a new version of MotecDataAnalyser so that **installed apps detect it and self-update**.

## Architecture in one paragraph

- **Private repo** `jducable00-gif/MotecDataAnalyser` — all source. Branches:
  - `advanced`: development branch (work happens here).
  - `release`: contains **only two files**, both with plain-ASCII names (`MotecDataAnalyser.exe`, `MotecDataAnalyser.mapping.json`) at the tip of a synthetic commit whose parent is the current `advanced` tip. No source, ever. The app loads its mapping from exe-relative `appDir + "MotecDataAnalyser.mapping.json"` — any non-ASCII/mojibake tree-entry name silently breaks channel mapping on fresh installs (this happened in v5.6.8's first dist commit).
- **Public repo** `jducable00-gif/MotecDataAnalyser-Releases` — distribution/update feed only (stable channel):
  - branch `main`: same two files as private `release`.
  - GitHub Releases (tagged `vX.Y.Z`) with the exe attached as asset named exactly `MotecDataAnalyser.exe`.
- **Public pre-release feed** `jducable00-gif/MotecDataAnalyser-PreReleases` — opt-in testers channel. Same shape as the stable public repo, but only apps that have `[General] PreReleaseUpdates=1` in their INI (Settings → "Receive pre-releases") ever query it. See [Pre-release channel](#pre-release-channel--separate-public-feed) below.
- The app ([ReleaseInfo.cpp](../ReleaseInfo.cpp)) does an **unauthenticated** GET on
  `https://api.github.com/repos/jducable00-gif/MotecDataAnalyser-Releases/releases/latest`,
  compares tags, and downloads the first `.exe` asset. Stable users query exactly that one repo; testers (`[General] PreReleaseUpdates=1`) get **both** feeds in parallel (one detached thread per repo) — see [Pre-release channel](#pre-release-channel--separate-public-feed). Since V5.6.9 it ALSO downloads mapping.json: [ReleaseInfo.cpp](../ReleaseInfo.cpp) derives its URL from the release tag as `https://raw.githubusercontent.com/jducable00-gif/MotecDataAnalyser-Releases/{tag}/MotecDataAnalyser.mapping.json`, so **the dist tree must always contain both files with clean ASCII names** — a missing/mojibake'd json name silently 404s the mapping download (exe update itself is non-blocking on that failure, but users keep stale channel mappings). Private repos return flat 404s to unauthenticated calls — that is why this public repo exists. **Never point the app back at the private repo.**

Version format: `VX.Y.Z`. The in-app comparison treats a missing segment as 0 (`V5.6` < `v5.6.1`).
The version literal lives in one place only: [`kAppVersion`](../ReleaseInfo.h) (e.g. `"V5.6.2"`).

## Pre-release channel — separate public feed

A second **public** repository, `jducable00-gif/MotecDataAnalyser-PreReleases`, is a standalone self-update feed for testers. It exists so you can ship and test versions without touching the stable release path (installed apps on the default/stable feed never see pre-releases).

How it works:
- The app's update check is driven by `[General] PreReleaseUpdates` in `MotecDataAnalyser.ini`:
  - `0` / absent → **one** fetch, stable feed only (`.../repos/jducable00-gif/MotecDataAnalyser-Releases/releases/latest`) — the default. Stable users never pay for a second request.
  - `1` (testers) → **both** feeds in parallel: one detached thread per repo (`fetchLatestReleasesAsync`, pre-release first). When all workers finish, results are merged and exactly ONE release is surfaced to the form: the **newest installable** across both feeds; on an exact version tie the pre-release feed wins (so a tester stays on their preview build until something strictly newer ships anywhere).
- The toggle is a checkbox in **Settings** ("Recevoir aussi les versions pré-release" / "Also receive pre-releases"). Flipping it and clicking OK re-fetches immediately from the newly selected repo set (`applyReleaseFeedChange`); no restart needed.

Tagging rule for pre-releases:
- The comparator ([compareVersions](../ReleaseInfo.cpp)) is **numeric-only**: suffixes like `-pre`, `rc1` are ignored, so `v5.7-pre == v5.7`. Tag a preview with the numeric part of what it previews (e.g. `v5.7-pre` for an upcoming 5.7).
- A pre-release is only "installable" while its number beats the installed app's version; once you ship a strictly higher stable, testers move to that stable automatically — no manual un-stick needed. Keep tagging newer previews as long as you want testers ahead of stable.
- Because `/releases/latest` excludes releases marked *pre-release*, channel separation is done by **repo**, not by GitHub's prerelease flag: publish normal (non-prerelease) Releases on `MotecDataAnalyser-PreReleases`.

Publishing a pre-release = the stable procedure with two substitutions:
1. Add/verify a remote for the feed repo if you don't have one:
   ```powershell
   git remote add dist-pre https://github.com/jducable00-gif/MotecDataAnalyser-PreReleases.git
   ```
2. Run steps 3–5 above, but push the two-file commit to `dist-pre` (not `dist`) and create/verify the GitHub Release + exe asset on **MotecDataAnalyser-PreReleases** (replace every `jducable00-gif/MotecDataAnalyser-Releases` in steps 4a–5 with `jducable00-gif/MotecDataAnalyser-PreReleases`).
   - Same two-file dist tree: plain-ASCII `MotecDataAnalyser.exe` + `MotecDataAnalyser.mapping.json`. The mapping download URL is derived from the tag as `https://raw.githubusercontent.com/jducable00-gif/MotecDataAnalyser-PreReleases/{tag}/MotecDataAnalyser.mapping.json`, so both files must be present with clean names.
   - Bump `kAppVersion` (step 1) before building, exactly like a stable release — the in-app "newer than installed" comparison is identical on either feed.

Notes:
- The pre-release repo needs no source and never receives it; keep it to dist content + Releases only, same as the stable public repo.
- An app left on the default (stable) feed will **never** detect a release that exists only in `MotecDataAnalyser-PreReleases` — testers are the ones who see both feeds (union), not the other way around. To promote a pre-release to everyone, publish it again through the normal stable procedure against `MotecDataAnalyser-Releases`.

## Prerequisites / environment notes

- Windows, PowerShell **5.1** (no `&&`, no `<` redirection — use pipes; git writes to stderr on success so red text + exit code 1 is normal for push output: judge by the `[new tag]`/branch lines).
- Build toolchain via [scripts/build.ps1](../scripts/build.ps1) (`.\scripts\build.ps1 -Config Release`) → `Win32\Release\Project1.exe`. If linking fails on a locked exe, kill running instances first: `$p = Get-Process Project1; if ($p) { $p | ForEach-Object { Stop-Process -Id $_.Id } }`.
- GitHub token for API calls (release creation + asset upload): use the **reusable PAT** stored at `.release_token.txt` in the repo root (classic `repo` scope, 90-day validity). It is git-excluded via `<git-dir>/info/exclude`, so it never enters commits - and must never be printed:

  ```powershell
  $tok = (Get-Content .release_token.txt -Raw).Trim()   # use in the Authorization header per step 4a/4b; log only length, never value
  ```

**Token freshness:** this PAT is reusable across calls and sessions - release creation + asset upload can be separate commands. If a call returns `401 Bad credentials`, the token has expired: ask the user for a fresh one (scope `repo`) and overwrite .release_token.txt. Do NOT use GCM (`git credential fill`) tokens as fallback: they are one-time OAuth grants that die after first use, so any multi-call flow 401s on call #2.


- Git remotes already configured locally: `origin` → private repo, `dist` → public dist repo. If missing:
  `git remote add dist https://github.com/jducable00-gif/MotecDataAnalyser-Releases.git`.
- **Filename caveat:** the product word can arrive corrupted when typed literally inside agent tool-call strings (a stray character gets inserted mid-word). Never type it in commands: derive names from `git ls-tree --name-only HEAD`, `(Get-ChildItem -Filter '*.exe').Name`, or wildcard filters; if a URL must be built inline, assemble the name at runtime (e.g. Python heredoc) and verify with `.Replace()` of any corrupted variant before sending.

## Procedure

Let `$TAG = vX.Y.Z`, e.g. `v5.6.3`. Work from the repo root on branch `advanced` with a **clean tree**.

### 1 — Bump version, commit, tag (private)

- Minor (`X.Y`) → use [scripts/release.ps1](../scripts/release.ps1):
  ```powershell
  .\scripts\release.ps1 v5.7     # bumps kAppVersion, commits "Release vX.Y", annotated tag, pushes branch + tag to origin
  ```
  It **rejects** `X.Y.Z` patch versions — for those do the same steps manually:

- Patch (`X.Y.Z`) → manual equivalent of release.ps1:
  1. Edit [`kAppVersion`](../ReleaseInfo.h) in [ReleaseInfo.h](../ReleaseInfo.h): `"V5.6.2"` → `"V5.6.3"`. (Normal edit tools work on this file.)
  2. `git add ReleaseInfo.h; git commit -m "Release v5.6.3"`
  3. `git tag -a v5.6.3 -m "Release v5.6.3"` (optional — see warning below)
  4. Push branch + tag: `git push origin advanced` and, only if the tag was created, `git push origin v5.6.3`.

> **⚠️ The private provenance tag is NOT for distribution.** It points at a full-source commit on `advanced`, so its GitHub "zip" download contains ALL source code — never share it or link to it with users (this caused confusion twice: v5.6.1 and v5.6.3). Users only ever get updates from the **public** repo's release assets / dist-`main` zip, which contain exe + mapping.json only. The tag is purely optional provenance metadata; skipping steps 3–4 entirely has zero effect on update detection (the app reads `/releases/latest` of the public repo).

### 2 — Build the release binary

```powershell
$p = Get-Process Project1 -ErrorAction SilentlyContinue; if ($p) { $p | ForEach-Object { Stop-Process -Id $_.Id } }
.\scripts\build.ps1 -Config Release     # expect: BUILD OK -> Win32\Release\Project1.exe, EXITCODE=0
```

### 3 — Create the release-content commit (two files only) and push to BOTH remotes

Build a tree containing exactly `MotecDataAnalyser.exe` (= fresh build) + mapping.json, parented on the new `advanced` tip:

```powershell
$exeSha = & git hash-object -w 'Win32\Release\Project1.exe'
$jsonSrc = (Get-ChildItem . -Filter '*mapping.json').FullName   # wildcard: mojibake-safe
$jsonSha = & git hash-object -w $jsonSrc
# tree-entry name MUST be plain ASCII — the app loads this exact filename next to the exe:
$entryName = 'MotecDataAnalyser.mapping.json'   # do NOT copy a mojibake'd name from an old HEAD

# ⚠️ Do NOT feed `git mktree` through a PowerShell pipeline, and do NOT use WriteAllLines for the spec file.
# Windows PowerShell 5.1 terminates every piped line with CRLF when handing it to native commands (and
# WriteAllLines writes CRLF too) — both leak a trailing \r into EVERY tree-entry name: `git ls-tree` shows
# quoted names like "MotecDataAnalyser.exe\r" and raw.githubusercontent.com 404s them silently. This is the
# v5.6.8 incident, reproduced verbatim by this command block in v5.6.13 before being caught.
$spec = Join-Path $env:TEMP 'rel.tree'   # LF-only bytes — WriteAllBytes, never WriteAllLines (CRLF)
[System.IO.File]::WriteAllBytes($spec, [System.Text.Encoding]::UTF8.GetBytes("100644 blob ${exeSha}`tMotecDataAnalyser.exe`n100644 blob ${jsonSha}`t${entryName}`n"))
# Helper script: reads the spec file ITSELF and feeds git mktree via subprocess raw stdin.
# Written with a single-quoted here-string so PowerShell does not touch its content, and it takes only
# one path argument (no embedded quotes survive command-line arg parsing — a classic PS pitfall).
$helper = Join-Path $env:TEMP 'mktree_lf.py'
Set-Content -Path $helper -Value @'
import sys, subprocess
data = open(sys.argv[1], "rb").read()
r = subprocess.run(["git", "mktree"], input=data, capture_output=True)
sys.stdout.write(r.stdout.decode().strip())
'@
$tree = (python $helper $spec).Trim()               # raw LF bytes in — no PS pipe

$msgFile = Join-Path $env:TEMP 'rel.msg'
[System.IO.File]::WriteAllText($msgFile, "Release v5.6.3 distribution build`n`nMotecDataAnalyser.exe + mapping.json only (no source). Update feed for the in-app self-update; published to public dist repo.")

$commit = (& git commit-tree $tree -p (git rev-parse HEAD) -F $msgFile).Trim()
& git update-ref refs/heads/release $commit                     # move local release branch
& git push --force origin release                               # private: force — synthetic tip is not a descendant of the old one
& git push --force dist "$($commit):refs/heads/main"            # public: same commit as main
```

Verify both sides show exactly two files with clean ASCII names (byte-level check — a stray `\r` in an entry name silently 404s raw.githubusercontent.com, the v5.6.8 incident). Quoted output from `git ls-tree --name-only` means non-ASCII bytes are present: run it and confirm NO quotes around either name:

```powershell
git fetch origin release; git fetch dist main   # refresh remote refs first — local tracking refs may be stale
$bad = @() + (git ls-tree --name-only origin/release) + (git ls-tree --name-only dist/main)
if ($bad | Where-Object { $_ -match '[^\x20-\x7E]' }) { Write-Host 'FAIL: non-clean tree entry name — do NOT publish, rebuild the commit with LF bytes' } else { Write-Host "OK: $($bad.Count) clean entries" }
```

> **CRLF note:** there are TWO distinct CRLF hazards. (1) File *content*: if you build this tree from files checked out locally (e.g. a clone of the dist repo), git's autocrlf may expand mapping.json to CRLF on disk — harmless: committing it back normalizes it, and `git commit` will report only the exe as changed. (2) Tree-entry *names*: PowerShell 5.1 pipes inject CRLF into native-command stdin (`Get-Content $spec | git mktree`) and `[System.IO.File]::WriteAllLines` writes CRLF line endings — both put a trailing `\r` inside every entry name, which is the v5.6.8/v5.6.13 incident class. Always write spec bytes with LF only (`WriteAllBytes`, as above) and feed git through Python subprocess raw stdin.
>
> If `git push --force origin release` ever fails with "send-pack: unexpected disconnect" (GitHub rejects pushing objects it does not have), fall back to the Git Data API on api.github.com, all authenticated with `$tok`: POST `/repos/.../git/blobs` (base64 content; large payloads via a temp file + `curl --data-binary @file`) → GET contents@release for existing blob SHAs and parent commit sha → POST `/git/trees` (`{"base_tree": <parent>, "tree":[{path,mode:"100644",type,"sha"}]}`) → POST `/git/commits` (message/tree/parents) → force-POST `refs/heads/release`. Build JSON payloads with a Python heredoc piped to stdin — inline PS double-quote strings mangle them.

### 4 — Publish the GitHub Release on the PUBLIC repo with the exe asset (API)

The app only reads **public** releases, so this is what makes an update visible. Do NOT create a release on the private repo for distribution purposes:

```powershell
$headers = @{ Authorization="Bearer $tok"; Accept='application/vnd.github+json' }   # $tok from Prerequisites; never print it

# 4a. Create the release (raw JSON string — do NOT pipe an already-JSON string through ConvertTo-Json, that double-encodes and yields a 422)
$bodyRaw = '{"tag_name":"v5.6.3","name":"V5.6.3 - Motec Data Analyser","body":"<release notes>","draft":false,"prerelease":false}'
$rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/jducable00-gif/MotecDataAnalyser-Releases/releases' `
        -Headers $headers -ContentType 'application/json; charset=utf-8' `
        -Body ([System.Text.Encoding]::UTF8.GetBytes($bodyRaw)) -Method Post

# 4b. Upload the binary to uploads.github.com through a Python helper (urllib — no PS pipe/encoding hazards,
#     and it verifies name+size in one shot). NOTE: GET on the release returns an EMPTY _links.assets.href -
#     construct this URL manually from $rel.id.
#     Derive BOTH the repo slug and asset name at runtime — never type them literally in a tool-call string
#     (the product word can arrive mojibake'd mid-word; v5.6.13 shipped `MotocataAnalyzeur.exe` this way).
$repoSlug = ((git remote get-url dist) -replace '^https?://[^/]+/', '') -replace '\.git$', ''
$exeName  = (git ls-tree --name-only origin/release | Where-Object { $_ -like '*.exe' }).Trim()
$url      = "https://uploads.github.com/repos/$repoSlug/releases/$($rel.id)/assets?name=" + [Uri]::EscapeDataString($exeName)
# Upload + verify in ONE Python helper (same pattern as the mktree helper above): PowerShell pipes and `2>&1`
# both corrupt binary/stderr streams around curl.exe — a stray stderr line makes ConvertFrom-Json fail, and an
# uninterpolated literal Authorization header yields {"message":"Requires authentication"} while curl still exits 0.
$up = Join-Path $env:TEMP 'upload_asset.py'
Set-Content -Path $up -Value @'
import json, os, sys, urllib.request
url, exe_path, expected_name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(exe_path, "rb").read()
req = urllib.request.Request(url, data=data, method="POST", headers={
    "Authorization": "Bearer " + os.environ["GH_TOKEN"],   # token via env var — never on a command line
    "Content-Type": "application/octet-stream"})
with urllib.request.urlopen(req) as r:
    resp = json.load(r)
ok_name, ok_size = resp.get("name") == expected_name, resp.get("size") == len(data)
print(json.dumps({"id": resp.get("id"), "name": resp.get("name"), "size": resp.get("size"),
                  "expectedName": expected_name, "localSize": len(data),
                  "ok": ok_name and ok_size}))
'@
$env:GH_TOKEN = $tok   # consumed by the helper; clear it afterwards ($env:GH_TOKEN = $null)
$r2 = (python $up $url 'Win32\Release\Project1.exe' $exeName | ConvertFrom-Json).Trim()
if (-not $r2.ok) { Write-Host "FAIL: asset [$($r2.name)] size $($r2.size) != expected [$exeName] — DELETE it (DELETE /repos/$repoSlug/releases/assets/$($r2.id)) and re-upload" } else { Write-Host "OK: $($r2.name), $([math]::Round($r2.size/1MB,1)) MB" }
```


Asset name must be exactly plain-ASCII **`MotecDataAnalyser.exe`** — never a mojibake'd variant; non-ASCII names break download URLs and user trust (v5.6.8's first dist commit shipped `Motoc???...mapping.json`). uploads.github.com also bypasses api.github.com rate limits, which 403 on large binary bodies sent to the API host directly.


### 5 — Verify end-to-end (unauthenticated, exactly what the app does)

```powershell
$r = Invoke-WebRequest -Uri 'https://api.github.com/repos/jducable00-gif/MotecDataAnalyser-Releases/releases/latest' `
        -UseBasicParsing -Headers @{ Accept='application/vnd.github+json'; UserAgent='motec-check' }   # expect HTTP 200, tag = $TAG
$j = $r.Content | ConvertFrom-Json
$exeAsset = ($j.assets | Where-Object { $_.name -like '*.exe' })[0]    # must exist with a browser_download_url
if (-not $exeAsset) { Write-Host 'FAIL: no exe asset on latest release'; exit 1 }
# name must be the clean ASCII one from step 3 (byte-exact, not just *.exe):
$expected = (git ls-tree --name-only origin/release | Where-Object { $_ -like '*.exe' }).Trim()
if ($exeAsset.name -ne $expected) { Write-Host "FAIL: asset name [$($exeAsset.name)] != expected [$expected] — delete + re-upload per step 4b"; exit 1 }

# download URL reachable (302 -> CDN 200, content-length == local exe size):
& curl.exe -sSIL $exeAsset.browser_download_url | Select-String '^HTTP|^content-length'
```

All green → done. Installed apps pick it up on next launch: About box shows the new release + "Update available"; clicking download replaces **MotecDataAnalyser.exe** in place AND overwrites `MotecDataAnalyser.mapping.json` with the version-matched copy from raw.githubusercontent.com at refs/tags/{tag} (V5.6.9+). Only both INI files are left untouched — they're user-specific.

## Invariants / gotchas

- **Deleting a release tag deletes its GitHub Release object and assets** (git push of the deleted ref removes them server-side). Recreating the bare tag does NOT restore it — you must re-run step 4a/4b via API. If you ever need to fix dist content in place, prefer force-pushing new commits/tags over deleting tags; if a delete was unavoidable, recreate the release immediately or self-update goes blind (it reads `/releases/latest`, which silently falls back to the previous version).
- **Never** commit source to `release`/dist `main`. Keep releases clean: the only asset that matters for updates is the exe (the app picks the first `.exe` asset); since V5.6.9 the app also downloads a version-matched `MotecDataAnalyser.mapping.json` from raw.githubusercontent.com at refs/tags/{tag}, so the dist tree must always contain BOTH files with clean ASCII names; only INI files are left untouched.
- **Never** create distribution Releases on the private repo — unauthenticated clients get 404s there. Private tags/commits are for provenance only.
- `kAppVersion` must be strictly greater than what is installed, or apps will report "up to date". Bump it in step 1 **before** building (step 2) so the binary reports its own version correctly.
- Force-pushes of `release` and dist `main` are expected: each release commit is a fresh synthetic tip parented on `advanced`, not a descendant of the previous one.
- If an asset/release was published wrong, fix it by deleting via API (`DELETE /repos/.../releases/{id}`, assets first) or re-uploading with `-X PUT` to the same asset URL — do not leave stale releases marked *Latest*.
