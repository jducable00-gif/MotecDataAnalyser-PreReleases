#!/usr/bin/env python3
# -*- coding: ascii -*-
"""
json_to_vehicle_db.py -- Convert data/reference/ams2_vehicles_db.json into the
readable in-code C++ tables (VehicleDbData_*.cpp) compiled straight into the exe.

This is the single source of truth for the Layer-1 vehicle DB pipeline:
    extract_ams2_bff.py  ->  ams2_vehicles_db.json  ->  THIS SCRIPT  ->  VehicleDbData_*.cpp + VehicleDbBuckets.h

Small tweaks no longer need any script at all : edit a numeric value directly in
the generated .cpp files and rebuild. Re-run this script only when the JSON is
re-extracted (or hand-edited) wholesale:

    python scripts/json_to_vehicle_db.py [input.json]   (default: data/reference/ams2_vehicles_db.json)

Output : one VehicleDbData_<XX>.cpp per letter-range bucket at project root, each
defining `const VehicleDbRow kAms2Vehicles_<XX>[]`, plus a manifest header
(VehicleDbBuckets.h) that aggregates all buckets so the loader (VehicleDb.cpp)
never needs to change when cars are added/removed. One aligned line per car ;
absent optional values use the sentinel -1.f (kVehicleNoValue in VehicleDbRow.h).

The script FAILS LOUDLY (non-zero exit, nothing written) on any schema surprise:
missing Mass/CGHeight, object-form CG range ({min,step,max}), non-identical brake
blocks per car, missing OptimumTempRange/Fade fields, duplicate normalized names,
or non-ASCII characters in a name.

Self-check : after rendering the C++ text it is parsed back and every value diffed
against the JSON (exact float32 bits) before anything is written to disk. Stale
VehicleDbData_*.cpp files from previous runs are removed automatically.
"""

import datetime
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IN = ROOT / "data" / "reference" / "ams2_vehicles_db.json"
MAX_ROWS_PER_FILE = 160          # keep each data file easy to open/edit in the IDE
NO_VALUE = -1.0                  # == kVehicleNoValue (absent optional value)

WHEELS = ("FL", "FR", "RL", "RR")

# Brake field order, matching the C++ VehicleDbRow::brake aggregate. The two torque-loss
# fractions are optional (-> -1.f when absent); opt temps and fade-range temps are required.
BRAKE_LABELS = ("optMinC", "optMaxC", "fadeLossBelow", "fadeLossAbove", "fadeLowC", "fadeHighC")


def f32(x):
    """Round a JSON number to exact float32 bits (what C++ will store)."""
    return struct.unpack("f", struct.pack("f", float(x)))[0]


def fmt_f(v):
    v = NO_VALUE if v is None else f32(v)
    r = repr(float(v))                       # shortest round-trip, e.g. '400.0', '0.56'
    has_exp = "e" in r.lower()               # scientific notation (defensive; not expected for our ranges)
    if "." in r and not has_exp:
        r = r.rstrip("0").rstrip(".")       # 1165.0 -> 1165 ; 0.5 stays 0.5
    return (r + ".f") if ("." not in r and not has_exp) else (r + "f")


def num_or_none(x):
    """JSON number -> float ; anything else (null/missing/object) -> None."""
    if isinstance(x, bool):                  # JSON true/false are ints in Python : reject
        return None
    if isinstance(x, (int, float)):
        return float(x)
    return None


def extract_car(key, entry):
    """Return a row dict for one car, or None when the entry has no physics.chassis.

    Raises ValueError on any schema surprise that must be resolved by hand first."""
    phys = entry.get("physics") if isinstance(entry, dict) else None
    ch = (phys or {}).get("chassis") if isinstance(phys, dict) else None
    if not isinstance(ch, dict):
        return None

    mass = num_or_none(ch.get("Mass"))
    cg_h = num_or_none(ch.get("CGHeight"))
    if mass is None:
        raise ValueError("%s : missing numeric Mass" % key)
    if cg_h is None:
        raise ValueError("%s : missing numeric CGHeight (required in the row format)" % key)

    def cg(field):
        v = ch.get(field)
        if isinstance(v, (dict, list)):      # object-form {min,step,max} adjustable CG
            raise ValueError(
                "%s : %s is an object range -- extend VehicleDbRow for "
                "adjustable-CG support before re-emitting" % (key, field))
        return num_or_none(v)

    row = {"name": key, "massKg": mass, "cgHeightM": cg_h,
           "cgRightM": cg("CGRightRange"), "cgRearM": cg("CGRearRange")}

    # --- brakes : all four wheels must carry the same set -------------------
    # Required per wheel: OptimumTempRange[min,max], FadeRangeBelow/Above.
    # Optional (-> -1.f when absent): FadeTorqueLossBelow/Above -- a few trucks
    # only model below-threshold fade, which is physically consistent and safe:
    # no active consumer reads the torque-loss fractions today.
    # If ANY wheel's required temps are missing/null (the extractor nulls values
    # it deems implausible), we cannot fabricate numbers -> row["brake"]=None and
    # main() skips this car with a warning, instead of baking garbage in.
    br = ch.get("brakes")
    if not isinstance(br, dict):
        raise ValueError("%s : missing 'brakes' object" % key)
    req_idx = (0, 1, 4, 5)                # optMin/optMax/fadeLow/fadeHigh are mandatory
    TEMP_MIN_C, TEMP_MAX_C = 0.0, 1500.0  # plausible brake-temperature band [deg C]

    def _temp_ok(v):
        """Finite AND within the physical range for a brake temperature."""
        return v is not None and isinstance(v, (int, float)) \
            and v == v and abs(v) != float("inf") and TEMP_MIN_C <= v <= TEMP_MAX_C

    ref_w, ref_vals = None, None          # reference wheel set to compare the others against
    for w in WHEELS:
        b = br.get(w)
        if not isinstance(b, dict):
            raise ValueError("%s : missing brakes.%s" % (key, w))
        rng = b.get("OptimumTempRange")
        raw = [num_or_none(rng[0]), num_or_none(rng[1])] \
              + [b.get(k) for k in ("FadeTorqueLossBelow", "FadeTorqueLossAbove")] \
            if isinstance(rng, list) and len(rng) >= 2 else None
        fade_lo, fade_hi = b.get("FadeRangeBelow"), b.get("FadeRangeAbove")
        raw += [fade_lo, fade_hi]         # -> order matches BRAKE_LABELS exactly
        # Plausibility guard: if any required temp is out of band (e.g. 38470 deg C from a
        # corrupt extraction), treat the whole car as having no usable brake data -> skip it.
        if raw is None or not all(_temp_ok(raw[i]) for i in req_idx):
            row["brake"] = None           # unusable / implausible brake data -> skip car later
            break
        vals = {lab: f32(NO_VALUE if x is None else x) for lab, x in zip(BRAKE_LABELS, raw)}  # absent optional -> -1.f
        if ref_w is None:
            ref_w, ref_vals = w, dict(vals)
        elif any(ref_vals[lab] != vals[lab] for lab in BRAKE_LABELS):
            detail = "; ".join("%s %s=%r vs %s=%r" % (lab, ref_w, ref_vals[lab], w, vals[lab])
                               for lab in BRAKE_LABELS if ref_vals[lab] != vals[lab])
            raise ValueError(
                "%s : brake blocks differ between wheels (%s vs %s): %s -- "
                "fix the source data / extractor before re-emitting" % (key, ref_w, w, detail))
    else:                                 # loop finished without a None break -> all four valid + identical
        row["brake"] = [ref_vals[k] for k in BRAKE_LABELS]

    eng = (phys or {}).get("engine") if isinstance(phys, dict) else None
    eng = eng if isinstance(eng, dict) else {}
    row["oilOptC"]   = num_or_none(eng.get("OptimumOilTemp"))
    row["oilWarnC"]  = num_or_none(eng.get("OilWarningTemp"))
    row["waterWarnC"]= num_or_none(eng.get("WaterWarningTemp"))

    meta = entry.get("metadata") if isinstance(entry, dict) else None
    alias = (meta or {}).get("Alias_name") if isinstance(meta, dict) else None
    row["alias"] = alias if isinstance(alias, str) and alias.strip() else ""
    return row


def normalize(name):
    """Same canonical form as Ams2VehicleDb::find(): lowercase + '_' -> ' '."""
    return name.lower().replace("_", " ")


# ---------------------------------------------------------------------------
# C++ rendering (one aligned line per car)
# ---------------------------------------------------------------------------

def row_values(row):
    """The 13 numeric fields in emission order: mass, cgH, cgR, cgL + brake[6] + engine temps."""
    return [row["massKg"], row["cgHeightM"], row["cgRightM"], row["cgRearM"]] \
         + list(row["brake"]) + [row["oilOptC"], row["oilWarnC"], row["waterWarnC"]]


def render_bucket(tag, rows, sha, stamp):
    """Render one bucket as full .cpp file text (rows already sorted by name)."""
    lines = []
    a = lines.append

    def hdr(text=""):
        if not text:
            a("//" + "-" * 75)
        else:
            a("// " + text)

    first, last = rows[0]["name"], rows[-1]["name"]
    hdr("VehicleDbData_%s.cpp -- AMS2 vehicle DB (Layer 1), %d cars : [%s .. %s]" % (tag, len(rows), first, last))
    hdr("")
    hdr("GENERATED by scripts/json_to_vehicle_db.py from data/reference/ams2_vehicles_db.json")
    hdr("(sha256:%s, generated %s). Pipeline: extract_ams2_bff.py -> JSON -> this script." % (sha, stamp))
    hdr("DO NOT reformat. Hand-editing numeric values in place is the supported way to tweak a car ;")
    hdr("-1.f means 'absent' (kVehicleNoValue) for CG and engine-temperature fields.")
    hdr("")
    hdr("Column order : name, alias, massKg, cgHeightM, cgRightM, cgRearM,")
    hdr("               brake{optMinC,optMaxC,fadeLossBelow,fadeLossAbove,fadeLowC,fadeHighC},")
    hdr("               oilOptC, oilWarnC, waterWarnC   (brake set shared by FL/FR/RL/RR)")
    a("#include \"VehicleDbRow.h\"")
    a("")

    name_w = max(len(r["name"]) for r in rows) + 2          # room for quotes
    alias_w = max(len(r["alias"]) for r in rows) + 2             # room for quotes
    num_ws = ([max(len(fmt_f(row_values(r)[i])) for r in rows) for i in range(4)]
              + [0] * 6                                      # brake cells: no alignment inside braces
              + [max(len(fmt_f(row_values(r)[10 + j])) for r in rows) for j in range(3)])

    def fmt_row_body(r):
        """Corps de la ligne `{ ... }` : name, alias (string cells alignees)
        puis les 13 valeurs numeriques. La virgule separatrice est ajoutee
        par l'appelant AVANT tout commentaire eventuel."""
        vals = row_values(r)
        parts = ['"%s"' % r["name"].ljust(name_w - 2),                 # name cell (aligned)
                 '"%s"' % r["alias"].ljust(alias_w - 2),              # alias (Aligned_name du JSON)
                 ] + [fmt_f(v).rjust(num_ws[i]) for i, v in enumerate(vals[:4])] \
              + ["{ " + ", ".join(fmt_f(v) for v in vals[4:10]) + " }"] \
              + [fmt_f(v).rjust(num_ws[i]) for i, v in zip(range(10, 13), vals[10:])]
        return "{ " + ", ".join(parts) + " }"

    # "extern" is REQUIRED here : in C++ a top-level const array has INTERNAL linkage,
    # but the manifest (VehicleDbBuckets.h) declares it extern so VehicleDb.cpp can see it.
    # Without this keyword every bucket would be an undefined symbol at link time.
    a("extern const VehicleDbRow kAms2Vehicles_%s[] =" % tag)
    a("{")
    for i, r in enumerate(rows):
        sep = "," if i < len(rows) - 1 else ""
        a(fmt_row_body(r) + ("," if sep else ""))
    a("};   // %d vehicles" % len(rows))
    # Count symbol : sizeof is valid HERE (the array is DEFINED in this TU -> complete type).
    # "extern" est OBLIGATOIRE : un const top-level a par defaut une linkage INTERNE ; sans ce
    # mot-cle le manifeste (VehicleDbBuckets.h, inclus dans VehicleDb.cpp) ne trouverait pas le
    # symbole au link. Le .cpp de donnees definit donc explicitement "extern".
    a("")
    a("// Nombre de lignes du bucket (sizeof valide ici : tableau defini dans ce fichier).")
    a("extern const std::size_t kAms2VehicleCount_%s = sizeof(kAms2Vehicles_%s)/sizeof(VehicleDbRow);" % (tag, tag))
    return "\n".join(lines) + "\r\n"


def render_manifest(buckets, sha, stamp):
    """Render VehicleDbBuckets.h : extern decls per bucket + aggregate table."""
    lines = []
    a = lines.append

    def hdr(text=""):
        if not text:
            a("//" + "-" * 75)
        else:
            a("// " + text)

    total = sum(len(rs) for _, rs in buckets)
    hdr("VehicleDbBuckets.h -- GENERATED by scripts/json_to_vehicle_db.py (sha256:%s, %s)." % (sha, stamp))
    hdr("Manifest of the AMS2 vehicle DB data files (%d cars). DO NOT EDIT BY HAND." % total)
    a("#pragma once")
    a("")
    a('#include "VehicleDbRow.h"')   # apporte aussi <cstddef> -> std::size_t
    a("")
    for tag, rows in buckets:
        first, last = rows[0]["name"], rows[-1]["name"]
        a("extern const VehicleDbRow kAms2Vehicles_%s[];   // %3d cars : [%s .. %s]"
          % (tag, len(rows), first, last))
        # count symbol defini dans le .cpp de donnees (sizeof valide la-bas)
        a("extern const std::size_t kAms2VehicleCount_%s;" % tag)
    a("")
    a("// Aggregate table so the loader never needs to change when buckets are added/renamed.")
    a("struct Ams2BucketRef { const char* tag; std::size_t count; const VehicleDbRow* rows; };")
    a("")
    a("namespace {   // internal linkage : include this header from ONE translation unit only (VehicleDb.cpp)")
    a("const Ams2BucketRef kAms2Buckets[] =")
    a("{")
    for i, (tag, _) in enumerate(buckets):
        sep = "," if i < len(buckets) - 1 else ""
        # count via le symbole exporte par le .cpp de donnees (pas sizeof : type incomplet ici)
        a('    { "%s", kAms2VehicleCount_%s, kAms2Vehicles_%s }%s' % (tag, tag, tag, sep))
    a("};")
    a("}   // namespace")
    return "\n".join(lines) + "\r\n"


# ---------------------------------------------------------------------------
# Self-check : parse the rendered C++ back and diff against the JSON rows.
# ---------------------------------------------------------------------------

LINE_RE = re.compile(r'^\s*\{\s*"([^"]+)"(.*)$', re.M)
# Matches both "123.45f" / ".75f" (digits around dot) AND "1165.f" (integer with trailing dot).
FLOAT_RE = re.compile(r'[-+]?(?:[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?|[0-9]+\.)f')


def split_cells(body):
    """Split the part of a row after the name into top-level cells (braces-aware)."""
    # strip trailing ' }', optional comma and any // comment first
    body = re.sub(r'\}\s*,?\s*(//.*)?$', '', body)
    cells, depth, cur = [], 0, []
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == "," and depth == 0:
            cells.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    return (cells + [tail]) if tail else cells


def count_top_commas(body):
    """Nombre de virgules au niveau 0 (hors accolades) dans le corps d'une ligne."""
    n, depth = 0, 0
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        elif ch == "," and depth == 0:
            n += 1
    return n


def parse_back(text):
    """Parse rendered rows back into (name, [13 floats]) tuples."""
    out = []
    for m in LINE_RE.finditer(text):
        name = m.group(1).strip()   # strip column-alignment padding from ljust
        vals = []
        for c in split_cells(m.group(2)):
            # findall : a plain cell yields one float ; the brace-wrapped brake set six.
            vals.extend(float(s.rstrip("f")) for s in FLOAT_RE.findall(c))
        out.append((name, vals, count_top_commas(m.group(2))))
    return out


def self_check(rows_by_bucket, rendered):
    """Diff every emitted value against the source rows (exact float32 bits)."""
    problems = []
    for tag, text in rendered.items():
        parsed = parse_back(text)
        src = {r["name"]: row_values(r) for r in rows_by_bucket[tag]}
        if len(parsed) != len(src):
            problems.append("%s : %d lines parsed vs %d source cars" % (tag, len(parsed), len(src)))
            continue
        # Garde structurelle : chaque ligne doit porter EXACTEMENT 8 virgules de niveau 0 dans le
        # corps apres le nom -> ",A ,B ,C ,D ,{jeu freins} ,K ,L ,M" (les 5 seps internes du jeu
        # de freins sont a profondeur >=1, non comptes). Le findall ci-dessous est aveugle aux
        # virgules manquantes ; ce comptage les attrape.
        for name, vals, commas in parsed:
            if commas != 9:
                problems.append("%s : %r has %d top-level commas (expected 9) -- invalid C++ row" % (tag, name, commas))

        seen = set()
        for name, vals, _commas in parsed:
            key = next((k for k in src if normalize(k) == normalize(name)), None)
            if key is None or key in seen:
                problems.append("%s : line does not match a source car: %r" % (tag, name))
                continue
            seen.add(key)
            exp = [f32(NO_VALUE if v is None else v) for v in src[key]]   # None -> -1.f (matches fmt_f rendering)
            got = vals[:13] + [-1.0] * max(0, 13 - len(vals)) if len(vals) < 13 else vals[:13]
            if len(got) != 13:
                problems.append("%s : %r has %d values, expected 13" % (tag, name, len(vals)))
                continue
            for i in range(13):
                e = exp[i]
                g = got[i]
                if f32(e) != g:
                    problems.append("%s : %r field[%d] emitted %.9g expected %s"
                                    % (tag, name, i, g, repr(f32(e))))
    return problems


# ---------------------------------------------------------------------------

def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_IN
    raw = src.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()[:16]
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict):
        sys.exit("FATAL : top level of %s is not an object" % src)

    rows_all = []          # (name, rowdict), sorted by normalized name
    skip_chassisless = 0   # entries with no chassis block at all
    skip_badbrake = 0      # cars whose brake temps were nulled as implausible
    for key in data:
        try:
            r = extract_car(key, data[key])
        except ValueError as e:
            sys.exit("FATAL : " + str(e))
        if r is None:
            skip_chassisless += 1
            continue
        # Cars whose brake temps were nulled by the extractor (implausible values):
        # we cannot fabricate numbers -> skip with a warning instead of baking garbage.
        if r.get("brake") is None:
            print("WARN : skipping %r -- no valid brake temperature data "
                  "(extractor nullled implausible OptimumTempRange/FadeRange)" % key)
            skip_badbrake += 1
            continue
        # ASCII-only names (bcc32c encodes narrow literals as CP1252) ; no quote/backslash.
        for ch in key:
            o = ord(ch)
            if o > 0x7E or ch in ('"', "\\"):
                sys.exit("FATAL : name %r contains non-ASCII/quote chars -- fix the JSON first" % key)
        rows_all.append((key, r))

    rows_all.sort(key=lambda kv: normalize(kv[0]))
    norm_seen = {}
    for key, _ in rows_all:
        n = normalize(key)
        if n in norm_seen:
            sys.exit("FATAL : duplicate normalized name %r (%s and %s)" % (n, norm_seen[n], key))
        norm_seen[n] = key

    # Bucket by letter range so each file stays small enough to edit comfortably.
    buckets = []           # list of [tag_prefix_letter, rows...]
    for i in range(0, len(rows_all), MAX_ROWS_PER_FILE):
        chunk = rows_all[i:i + MAX_ROWS_PER_FILE]
        a, b = chunk[0][0][0].upper(), chunk[-1][0][0].upper()
        tag = (a + "_" + b) if a != b else a
        buckets.append((tag, [r for _, r in chunk]))

    stamp = datetime.date.today().isoformat()
    rendered = {tag: render_bucket(tag, rows, sha, stamp) for tag, rows in buckets}
    problems = self_check({t: rs for t, rs in buckets}, rendered)
    if problems:
        sys.exit("SELF-CHECK FAILED (%d problem(s)) -- nothing written:\n  %s"
                 % (len(problems), "\n".join(problems[:20])))

    # Remove stale data files from previous runs before writing the new set.
    for old in sorted(ROOT.glob("VehicleDbData_*.cpp")):
        tag = old.stem[len("VehicleDbData_"):]
        if tag not in rendered:
            old.unlink()
            print("  removed stale %s" % old.name)

    manifest = render_manifest(buckets, sha, stamp)
    (ROOT / "VehicleDbBuckets.h").write_bytes(manifest.encode("ascii"))   # hard fail on non-ASCII slip-through
    for tag, text in rendered.items():
        out_path = ROOT / ("VehicleDbData_%s.cpp" % tag)
        out_path.write_bytes(text.encode("ascii"))

    total = len(rows_all)
    skips = []
    if skip_chassisless:
        skips.append("%d without a usable chassis block" % skip_chassisless)
    if skip_badbrake:
        skips.append("%d with implausible brake temps (nullled by extractor)" % skip_badbrake)
    print("OK : wrote %d data files + VehicleDbBuckets.h (%d vehicles%s)"
          % (len(buckets), total, ", skipped " + " and ".join(skips) if skips else ""))
    for tag, rows in buckets:
        first, last = rows[0]["name"], rows[-1]["name"]
        print("  VehicleDbData_%s.cpp : %3d cars   [%s .. %s]" % (tag, len(rows), first, last))


if __name__ == "__main__":
    main()
