#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Extract AMS2 physics data from every .bff car pack.

Pipeline per car (see /memories/session/plan.md):
  1. Unpack the BFF with `D:\AMS2\AMS2_Mod\PCTools\unpack AMS2_car.bat`
     (interactive: pipes "<car name>\r\ny\r\n" on stdin -> answers Y to the
      "extraire tous les fichiers BFF generaux ?" question so the bat copies
      this car's .CRD into its freshly created <car> folder).
  2. Decode [PCTools]\\<car>\\vehicles\\physics\\chassis\\*.CDFBIN and
     \engines\\*.EDFBIN with ams2_cdf_decoder (port of ExtractData()).
  3. Merge results into data/reference/ams2_vehicles_db.json under the car's
     "physics" key, then delete ONLY this run's own artifacts before moving on.

DB keys are the display names read from each pack's .CRD file (<prop name=
"Vehicle Name">); the BFF stem is kept in metadata.bff_name for provenance.
Duplicates: last one wins + warnings recorded on both entries. Missing CRD /
name -> falls back to the BFF stem with a warning.
Cleanup is STRICTLY scoped: before each unpack we take a snapshot of PCTools
and afterwards delete only entries that did not exist in that snapshot (this
car's folder + its *_extracted working dirs).  Other pre-existing content —
general *_extracted working dirs, cars' folders, tool folders, loose files —
is never touched. --fresh backs up and rebuilds from {}.

Stdlib only. Usage:
    python scripts\extract_ams2_bff.py                 # all cars in Steam dir
    python scripts\extract_ams2_bff.py --car Alpine_A110_GT4_Evo   # one car
    python scripts\extract_ams2_bff.py --fresh         # rebuild DB from scratch
"""

import argparse
import html
import json
import os
import shutil
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ams2_cdf_decoder import FieldSpec, extract_field, DecodeError  # noqa: E402

# ---------------------------------------------------------------------------
# Paths (exe-relative to project root; Steam/PCTools are fixed external paths)
# ---------------------------------------------------------------------------
PROJECT_ROOT = Path(__file__).resolve().parents[1]
DB_FILE = PROJECT_ROOT / "data" / "reference" / "ams2_vehicles_db.json"
WORK_DIR = PROJECT_ROOT / "data" / "working"

STEAM_VEHICLES_DIR = (r"C:\Program Files (x86)\Steam\steamapps\common"
                      r"\Automobilista 2\Pakfiles\Vehicles")
PCTOOLS_DIR = Path(r"D:\AMS2\AMS2_Mod\PCTools")
UNPACK_BAT = PCTOOLS_DIR / "unpack AMS2_car.bat"

# ---------------------------------------------------------------------------
# Field identifiers (4-byte LE ids, from GeneralFunction.cpp::ExtractData)
# ---------------------------------------------------------------------------
CHASSIS_FIELDS = {
    # name: id bytes
    "Mass":            b"\x67\x0B\x57\xAB",   # int32  [kg]
    "CGHeight":        b"\x18\x24\xEA\xA8",   # float  [m]
    "CGRightRange":    b"\xDF\x8D\x93\xCF",   # f,f,f (min,step,max)
    "CGRightSetting":  b"\x00\x9D\x8A\xCF",   # value or NO DATA(0x28)
    "CGRearRange":     b"\xBE\xBA\x67\x7B",   # f,f,f (min,step,max)
    "CGRearSetting":   b"\xD4\x4C\x53\xC4",   # value or NO DATA(0x28)
}

# Brake corner section markers: raw 5-byte delimiters in the CDFBIN.
BRAKE_MARKERS = {
    "FL": bytes.fromhex("E07D16E29F"),
    "FR": bytes.fromhex("E0D6537908"),
    "RL": bytes.fromhex("E0BF9F5BA2"),
    "RR": bytes.fromhex("E0CFF2C932"),
}

BRAKE_FIELDS = {  # per corner, searched only inside that corner's slice
    "OptimumTempRange":       b"\x92\xAA\x06\x24",   # f,f (min,max) [deg C]
    "FadeTorqueLossBelow":    b"\x02\x3F\x01\xF1",   # float fraction 0..1
    "FadeTorqueLossAbove":    b"\x45\x81\x36\x93",   # float fraction 0..1
    "FadeRangeBelow":         b"\xE7\x95\xA8\xD0",   # f,f (min,max) [deg C]
    "FadeRangeAbove":         b"\xD0\x03\x18\x55",   # f,f (min,max) [deg C]
}

ENGINE_FIELDS = {
    "OptimumOilTemp":     b"\xAF\xD7\x8A\xDD",   # float [deg C]
    "OilWarningTemp":     b"\x3F\x6B\x7B\xE7",   # float [deg C]
    "WaterWarningTemp":   b"\x6D\x47\xC1\xB2",   # float [deg C]
}

# Plausibility bounds for brake-temperature sanity checks (see _brake_temp_ok).
BRAKE_TEMP_PLAUSIBLE_C = (0.0, 1500.0)


def _is_num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool) \
        and x == x and abs(x) != float("inf")


def _brake_temp_ok(v):
    """True when a brake temperature [deg C] is finite AND in the plausible band.

    Guards against id-matches that land on unrelated bytes (e.g. some kart BFFs
    where OptimumTempRange decodes to 38470 / 1057104384). Returns False for any
    non-numeric or out-of-band value so the caller can null it + warn loudly.
    """
    return _is_num(v) and BRAKE_TEMP_PLAUSIBLE_C[0] <= v <= BRAKE_TEMP_PLAUSIBLE_C[1]


def _brake_pair_ok(pair):
    """True when a [min,max] temp pair is present, numeric AND both in band."""
    return (isinstance(pair, list) and len(pair) == 2
            and all(_brake_temp_ok(v) for v in pair))


# ---------------------------------------------------------------------------
# CRD display-name extraction (DB keys)
# ---------------------------------------------------------------------------

def parse_crd(path: Path):
    """Extract the vehicle DISPLAY name from an AMS2 .CRD file.

    Looks for <prop ... name="Vehicle Name" data="...">.  ElementTree is tried
    first (handles XML entities); a regex fallback covers malformed markup.
    Returns str or None when no usable value is found.
    """
    try:
        raw = path.read_bytes()
    except OSError as e:                       # pragma: no cover
        log(f"  [!] cannot read CRD {path.name}: {e}")
        return None
    text = raw.decode("utf-8", errors="replace")
    try:
        import xml.etree.ElementTree as ET     # stdlib, local to keep top tidy
        root = ET.fromstring(text)
        for el in root.iter():
            if (el.tag == "prop"
                    and el.get("name", "").strip().lower() == "vehicle name"):
                data = (el.get("data") or "").strip()
                if data:                       # skip empty placeholders, keep looking
                    return data
    except ET.ParseError:                      # malformed -> regex fallback below
        pass
    for tag in re.findall(r'<prop\s+[^>]*/?>', text, re.I):
        n = re.search(r'name=["\']([^"\']*)["\']', tag)
        if not n or n.group(1).strip().lower() != "vehicle name":
            continue
        d = re.search(r'data=["\']([^"\']*)["\']', tag)
        name = html.unescape(d.group(1)).strip() if d else ""
        if name:                               # skip empty placeholders, keep looking
            return name
    return None


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _single(values):
    """Collapse a 1-element decode list to its scalar; None stays None."""
    if values is None:
        return None
    return values[0] if len(values) == 1 else values


def log(msg):    print(datetime.now().strftime("[%H:%M:%S] ") + msg)


# ---------------------------------------------------------------------------
# Phase B: file parsers
# ---------------------------------------------------------------------------

def parse_chassis(data: bytes, warnings: list) -> dict:
    """Decode a chassis CDFBIN into the physics payload."""
    out = {}

    for name, idb in CHASSIS_FIELDS.items():
        spec = FieldSpec(name, idb)
        try:
            values, _off = extract_field(data, spec)
        except LookupError as e:
            warnings.append(str(e))
            continue
        except DecodeError as e:
            warnings.append(f"{name}: decode error ({e})")
            continue

        if name in ("CGRearRange", "CGRightRange"):
            # Keep the raw f,f,f triple for now; final shape (scalar vs {min,step,max}) is decided below based on the matching *Setting.
            out[name] = list(values) if values else None
        else:  # Mass / CGHeight / *Setting (setting may be NO DATA -> None)
            out[name] = _single(values)

    # --- resolve Range shape from its Setting ---------------------------------
    # Setting == null → fixed position, store scalar (= min of the triple).
    # Setting != null → adjustable parameter, keep {min, step, max}.
    for range_key, setting_key in (("CGRearRange", "CGRearSetting"), ("CGRightRange", "CGRightSetting")):
        raw = out.get(range_key)
        if not isinstance(raw, list):
            continue  # missing / already scalar — leave as-is (None or value[0] fallback below)
        setting = out.get(setting_key)
        if setting is None:
            out[range_key] = raw[0]
        else:
            out[range_key] = {"min": raw[0], "step": raw[1], "max": raw[2]}

    # --- brake corners, bounded by the raw section markers -----------------
    marker_offsets = {}
    for corner, mk in BRAKE_MARKERS.items():
        idx = data.find(mk)
        if idx < 0:
            warnings.append(f"brake marker {corner} not found")
            continue
        marker_offsets[corner] = idx

    # slice per corner: from its own marker to the next one (or EOF).
    slices = {}
    ordered_offs = sorted(marker_offsets.values())
    by_off = {off: corner for corner, off in marker_offsets.items()}
    for i, off in enumerate(ordered_offs):
        end = ordered_offs[i + 1] if i + 1 < len(ordered_offs) else len(data)
        slices[by_off[off]] = (off, end)

    brakes = {}
    for corner, mk in BRAKE_MARKERS.items():
        cdata = {}
        found_any = False
        if corner in slices:
            start, end = slices[corner]
            search_start = start + len(mk)      # fields live after the marker
            for name, idb in BRAKE_FIELDS.items():
                spec = FieldSpec(f"{name}_{corner}", idb)
                try:
                    values, _off = extract_field(data, spec,
                                                 search_start=search_start,
                                                 search_end=end)
                except LookupError as e:
                    warnings.append(str(e))
                    continue
                except DecodeError as e:
                    warnings.append(f"{name} {corner}: decode error ({e})")
                    continue
                if values is None:              # 0x28 "Not Used"
                    cdata[name] = {"value": "not_used"}
                elif name.endswith("Range"):    # f,f pair -> [min, max], plausibility-checked
                    pair = list(values)
                    if _brake_pair_ok(pair):
                        cdata[name] = pair
                    else:                       # implausible (e.g. kart BFF offset bug)
                        warnings.append(
                            "implausible %s for brake corner %s (%r); nulled"
                            % (name, corner, pair))
                elif name.startswith("FadeTorqueLoss"):   # fraction 0..1
                    v = _single(values)
                    if _is_num(v) and -0.5 <= v <= 2.0:
                        cdata[name] = v
                    else:                       # implausible torque-loss fraction
                        warnings.append(
                            "implausible %s for brake corner %s (%r); nulled"
                            % (name, corner, v))
                else:                           # scalar float
                    cdata[name] = _single(values)
                found_any = True

        brakes[corner] = cdata if found_any else {}   # missing marker => {} + warning

    out["brakes"] = brakes
    return out


def parse_engine(data: bytes, warnings: list) -> dict:
    """Decode an engine EDFBIN into the physics payload."""
    out = {}
    for name, idb in ENGINE_FIELDS.items():
        spec = FieldSpec(name, idb)
        try:
            values, _off = extract_field(data, spec)
        except LookupError as e:
            warnings.append(str(e))
            continue
        except DecodeError as e:
            warnings.append(f"{name}: decode error ({e})")
            continue
        out[name] = _single(values)
    return out


# ---------------------------------------------------------------------------
# Phase C/D: unpack orchestration + DB merge
# ---------------------------------------------------------------------------

def run_unpack(car_name: str, timeout_s: int):
    """Run the interactive batch for one car; answer Y to general-files Q.

    Answering Y makes the bat extract the general BFFs (BOOTFLOW,
    PHYSICSPERSISTENT, ...) and copy this car's .CRD / images into its fresh
    <car> folder.  Returns (returncode:int|None, log_text:str); returncode
    None = timed out.
    """
    stdin_data = f"{car_name}\r\ny\r\n"
    try:
        proc = subprocess.run(
            [str(UNPACK_BAT)],
            input=stdin_data.encode("ascii"),
            capture_output=True,
            timeout=timeout_s,
            cwd=str(PCTOOLS_DIR),
        )
        rc = proc.returncode
        out_b = (proc.stdout or b"") + (b"\n[stderr]\n" if proc.stderr else b"") \
                + (proc.stderr or b"")
    except subprocess.TimeoutExpired as e:
        return None, f"[timeout after {timeout_s}s] " + \
            ((e.output or b"").decode(errors="replace"))

    log_text = out_b.decode("cp850", errors="replace") if rc is not None else ""
    # keep only the tail to bound file size
    return rc, (log_text[-20_000:] if len(log_text) > 20_000 else log_text)


# PCarsTools names its output folder after the INTERNAL vehicle id, not the
# BFF name: Clio_Cup14.bff -> [Kart_01]\..., mini_coopers_SPEC.bff ->
# [BMW_2002_Turbo]\....  So we cannot look in PCTOOLS_DIR/<car>.
#
# Strategy (mtime diffs are unreliable — PCarsTools preserves the original
# file timestamps from inside the BFF, so a stale leftover can have an old
# mtime and pass any "recently modified" test):
#   * _snapshot_pctools() records every PCTOOLS_DIR entry BEFORE the unpack;
#   * after it, only entries ABSENT from that snapshot belong to THIS run —
#     those are used for discovery AND they (only) get deleted afterwards.
def list_physics_files(root: Path = None) -> list:
    """All extracted car data (CDFBIN/EDFBIN/.CRD) under PCTools, sorted."""
    base = root if root is not None else PCTOOLS_DIR
    out = []
    for pat in ("*.CDFBIN", "*.EDFBIN", "*.crd"):
        try:
            it = base.rglob(pat)
            for p in sorted(it):
                if p.is_file():
                    out.append(p)
        except OSError as e:                 # pragma: no cover
            log(f"  [!] extracted-data scan failed ({pat}): {e}")
    return sorted(set(out), key=str)


def _snapshot_pctools() -> set:
    """Names of every top-level entry in PCTOOLS_DIR (baseline for a run)."""
    try:
        return {p.name for p in PCTOOLS_DIR.iterdir()}
    except OSError as e:                     # pragma: no cover
        log(f"  [!] cannot snapshot PCTools: {e}")
        return set()


def _cleanup_run_artifacts(car_name: str, pre_snapshot: set) -> bool:
    """Delete ONLY the top-level entries this run created in PCTOOLS_DIR.

    Everything that was already there before the unpack (other cars' folders,
    tool folders such as 02_bmt2xml, loose files of any kind) is left exactly
    as found.  Retries a few times — Windows file locks can linger right after
    an unpack.  Returns True when nothing from this run remains.
    """
    for attempt in range(1, 4):
        try:
            entries = list(PCTOOLS_DIR.iterdir())
        except OSError as e:                 # pragma: no cover
            log(f"  [!] cannot list PCTools for cleanup: {e}")
            return False
        new_entries = [p for p in entries if p.name not in pre_snapshot]
        for p in new_entries:
            try:
                if p.is_dir():
                    shutil.rmtree(p, ignore_errors=True)
                else:
                    p.unlink()
            except Exception as e:           # pragma: no cover
                log(f"  [!] could not remove {p.name}: {e}")
        remaining = [p for p in new_entries if p.exists()]
        if not remaining:
            return True
        log(f"  [!] {car_name}: this run's files still present after cleanup "
            f"attempt {attempt}/3 — retrying")
        time.sleep(1)
    leftovers = sorted(p.name for p in new_entries)[:5]
    log(f"  [!!] {car_name}: could not remove all of this run's files: {leftovers}")
    return False


def _startup_check() -> bool:
    """Verify PCTOOLS_DIR is accessible.  Nothing is deleted here.

    All cleanup is handled per-car by ``_cleanup_run_artifacts`` using a
snapshot diff — only entries that did NOT exist before the car's unpack are
ever removed.  Pre-existing content (general *_extracted working dirs, tool
folders, loose files) stays untouched for the entire session and across runs.
    """
    try:
        list(PCTOOLS_DIR.iterdir())
    except OSError as e:                     # pragma: no cover
        log(f"  [!] cannot access PCTools at {PCTOOLS_DIR}: {e}")
        return False
    return True


def process_car(bff_path: Path, db: dict, log_entries: list, timeout_s: int):
    """Full pipeline for one .bff. Returns True on success.

    The DB entry is keyed by the display name read from this pack's .CRD file;
    the BFF stem is stored in metadata.bff_name (provenance + legacy lookup).
    Cleanup is snapshot-based: only entries created DURING this run are ever
deleted afterwards — pre-existing PCTools content stays untouched.
    """
    car = bff_path.stem
    warnings = []

    # 1) unpack. Snapshot first so we can tell OUR artifacts from anything that
    # was already in PCTOOLS_DIR (which must never be deleted).
    pre_snapshot = _snapshot_pctools()
    rc, bat_log = run_unpack(car, timeout_s)
    if rc is None or rc != 0:
        entry = {"car": car, "status": "unpack_failed",
                 "returncode": rc,
                 "time": datetime.now(timezone.utc).isoformat()}
        log_entries.append(entry)
        _save_bat_log(car, bat_log)
        # remove only what this run created (car folder / *_extracted dirs /
        # temp .bff copies) so it cannot contaminate the NEXT car's snapshot.
        if not _cleanup_run_artifacts(car, pre_snapshot):
            entry["status"] = "unpack_failed_post_cleanup"
        return False

    new_files = list_physics_files()          # entries created by THIS run only

    # --- CRD: display name (DB key) -----------------------------------------
    # Per-car .CRDs live in the persistent BOOTFLOW_extracted working dir at
    #   PCTools\BOOTFLOW_extracted\vehicles\<car_id>\<id>.crd
    # where <car_id> is the lowercased BFF stem.  The bat does NOT copy them
    # into the car's target folder, so we must look there directly.
    crds = [p for p in new_files if p.suffix.lower() == ".crd"]
    crd_path = next((p for p in crds if p.stem.lower() == car.lower()), None)
    if crd_path is None:
        # Fallback (normal case): persistent BOOTFLOW working dir.
        bootflow_car_dir = PCTOOLS_DIR / "BOOTFLOW_extracted" / "vehicles" / car.lower()
        if bootflow_car_dir.is_dir():
            found = list(bootflow_car_dir.glob("*.crd"))
            crd_path = next((p for p in found if p.stem.lower() == car.lower()), None) \
                       or (found[0] if len(found) == 1 else None)
    display_name = parse_crd(crd_path) if crd_path else None
    key = car                                   # DB key (display name preferred)
    if not display_name:
        warnings.append("no 'Vehicle Name' found in CRD — using BFF stem as key")
    else:
        key = display_name                      # e.g. "Ginetta G55 GT4"

    cdfbin_path = next((p for p in new_files if p.suffix.upper() == ".CDFBIN"), None)
    edfbin_path = next((p for p in new_files if p.suffix.upper() == ".EDFBIN"), None)
    binaries = [p for p in new_files if p.suffix.lower() not in (".crd",)]
    if len(binaries) > 2:                       # expect at most CDFBIN + EDFBIN
        log(f"  [!] {len(binaries)} physics files found (expected <=2): "
            f"{[str(p.relative_to(PCTOOLS_DIR)) for p in binaries]}")

    physics = {}
    engine_out = {}
    if cdfbin_path is None:
        warnings.append("no new CDFBIN appeared after unpack")
    else:
        data = cdfbin_path.read_bytes()
        physics["chassis"] = parse_chassis(data, warnings)

    if edfbin_path is None:
        warnings.append("no new EDFBIN appeared after unpack")
    else:
        data = edfbin_path.read_bytes()
        engine_out["engine"] = parse_engine(data, warnings)

    if not cdfbin_path and not edfbin_path:
        entry = {"car": car, "status": "no_physics_files",
                 "time": datetime.now(timezone.utc).isoformat()}
        log_entries.append(entry)
        _save_bat_log(car, bat_log + "\n[warn] no physics files found")
        if not _cleanup_run_artifacts(car, pre_snapshot):
            entry["status"] += "+post_cleanup_failed"
        return False

    # 3) merge into DB (keyed by display name; BFF stem kept for provenance) --
    existing = db.get(key)
    if existing is not None and isinstance(existing, dict):
        old_bff = ((existing.get("metadata") or {}).get("bff_name")) or car
        if old_bff != car:
            dup_msg = (f"duplicate 'Vehicle Name' {key!r}: BFFs "
                       f"{old_bff} and {car}; last one wins")
            for e in (existing,):               # warn on the entry being replaced
                em = dict(e.get("metadata") or {})
                ew = list((e.get("physics") or {}).get("_warnings") or [])
                if dup_msg not in ew:
                    ew.append(dup_msg)
                e["physics"] = {**(e.get("physics") or {}), "_warnings": ew}
            warnings.append(f"duplicate 'Vehicle Name' with BFF {old_bff}; last one wins")
    car_entry = db.setdefault(key, {})
    meta = dict(car_entry.get("metadata") or {})
    if key != car:
        meta["bff_name"] = car                 # provenance: which BFF this came from
    merged = dict(car_entry.get("physics") or {})   # idempotent per-car overwrite
    files_map = dict(merged.get("_files") or {})

    if cdfbin_path is not None:
        merged["chassis"] = physics.get("chassis", {})
        files_map["chassis"] = str(cdfbin_path.name)
    if edfbin_path is not None:
        merged["engine"] = engine_out.get("engine", {})
        files_map["engine"] = str(edfbin_path.name)

    if files_map:
        merged["_files"] = files_map
    car_entry["physics"] = merged
    if warnings:                                # replace stale list on re-runs
        merged["_warnings"] = warnings
    else:
        merged.pop("_warnings", None)
    car_entry["metadata"] = meta

    entry = {"car": car, "status": "ok",
             "warnings_count": len(warnings),
             "time": datetime.now(timezone.utc).isoformat()}
    if warnings:
        entry["warnings"] = warnings[:20]
    log_entries.append(entry)
    _save_bat_log(car, bat_log + ("\n[warn]\n" + "\n".join(warnings)
                                  if warnings else ""))

    # 5) remove this run's artifacts (car folder + working dirs only).
    if not _cleanup_run_artifacts(car, pre_snapshot):
        entry["status"] += "+post_cleanup_failed"

    mass = (physics.get("chassis") or {}).get("Mass")
    oil = (engine_out.get("engine") or {}).get("OptimumOilTemp")
    log(f"  OK {car} | Mass={mass} kg, OptOil={oil}")
    return True


def _save_bat_log(car: str, text: str):
    try:
        WORK_DIR.mkdir(parents=True, exist_ok=True)
        (WORK_DIR / f"unpack_{car}.log").write_text(text, encoding="utf-8")
    except Exception as e:                       # pragma: no cover
        log(f"  [!] could not save bat log for {car}: {e}")


def load_db() -> dict:
    if DB_FILE.exists():
        try:
            return json.loads(DB_FILE.read_text(encoding="utf-8"))
        except Exception as e:
            raise SystemExit(f"[FATAL] cannot read existing DB "
                             f"{DB_FILE}: {e}")
    return {}


def save_db(db: dict):
    DB_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = DB_FILE.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(db, f, indent=4, ensure_ascii=False)
    os.replace(str(tmp), str(DB_FILE))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--car", help="process a single car (BFF basename)")
    ap.add_argument("--fresh",
                    action="store_true",
                    help="back up the existing DB and rebuild from scratch")
    ap.add_argument("--timeout", type=int, default=900,
                    help="per-car unpack timeout in seconds (default 900)")
    args = ap.parse_args()

    if not UNPACK_BAT.exists():
        raise SystemExit(f"[FATAL] batch script missing: {UNPACK_BAT}")
    if not Path(STEAM_VEHICLES_DIR).is_dir():
        raise SystemExit(
            f"[FATAL] Steam vehicles dir missing: {STEAM_VEHICLES_DIR}")

    bffs = sorted(Path(STEAM_VEHICLES_DIR).glob("*.bff"))
    if args.car:
        wanted = [b for b in bffs if b.stem == args.car]
        if not wanted:
            raise SystemExit(f"[FATAL] no BFF named {args.car!r} found")
        bffs = wanted
    # Refuse to start when pre-existing car folders / *_extracted dirs are
    # present: the snapshot diff would mis-attribute them.  Nothing is deleted
    # without an explicit 'y' from the user.
    if not _startup_check():
        raise SystemExit(
            "[FATAL] PCTools has pre-existing entries — start aborted (manual check needed)")

    log(f"{len(bffs)} car(s) to process. DB -> {DB_FILE}")
    if args.fresh and DB_FILE.exists():
        # Back up the current DB so a fresh run is auditable/reversible.
        WORK_DIR.mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = WORK_DIR / f"ams2_vehicles_db.pre-fresh-{ts}.json"
        shutil.copy2(str(DB_FILE), str(backup))
        log(f"--fresh: existing DB backed up to {backup.name}")
    db = {} if args.fresh else load_db()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    log_file = WORK_DIR / "bff_extract_log.json"

    prev_entries = []
    if log_file.exists():
        try:
            prev_entries = json.loads(log_file.read_text(encoding="utf-8"))
        except Exception:
            pass

    def _flush():
        """Persist DB + log after every car so a crash loses no progress.

        Log is deduped per-car (latest entry wins) so re-runs of individual
        cars don't accumulate duplicate lines.
        """
        save_db(db)
        latest = {}
        for e in prev_entries:
            if isinstance(e, dict) and "car" in e:
                latest[e["car"]] = e
        with open(log_file, "w", encoding="utf-8") as f:
            json.dump(list(latest.values()), f,
                      indent=2, ensure_ascii=False)

    ok = fail = 0
    for i, bff in enumerate(bffs, 1):
        log(f"[{i}/{len(bffs)}] {bff.stem}")
        if process_car(bff, db, prev_entries, args.timeout):
            ok += 1
        else:
            fail += 1
        _flush()

    _flush()

    log("=" * 50)
    log(f"Done. ok={ok} failed={fail}. DB updated ({len(db)} cars total).")


if __name__ == "__main__":
    main()
