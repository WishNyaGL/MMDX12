#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mme_compat_run.py - M7a MME compatibility harness.

Runs the MMDX12 MME runtime against the downloaded effect pack (captures/mme_lib) and writes
the compatibility report: per-effect static compile rate + one pinned-scene render run, an
automatic tier guess, a contact sheet, and docs/mme/compat_list.md.

Stages
  1. Discover: per catalog entry with status ok, find the effect's entry points (the .fx files
     in its src tree that another .fx does not #include), classify each with the app's own
     parser (build_mme/bin/mme_parse_test.exe --summary), and pick a primary entry point.
  2. Static: compile the chosen entry points with build_mme/bin/mme_compile_test.exe (object mode
     for object techniques). No window.
  3. Render: one app window per effect, strictly sequential, timeout 180 s each, on the pinned
     scene, capturing to captures/mme_compat/<rank>_<id>.png and copying the log.
  4. Report: captures/mme_compat/results.json, docs/mme/compat_list.md, captures/mme_compat/sheet.png.

Design: docs/mme/mme_design.md §4 (tiers), §6 (test strategy), §5 M7. Library pack: captures/mme_lib/.

Usage
    python -I tools/mme/mme_compat_run.py [--only 13,70,6] [--resume] [--limit N] [--all-variants]
                                         [--static-only] [--no-static] [--sheet-only]

The pack under captures/mme_lib is untrusted third-party content: run this script with `python -I`
(isolated mode) and never execute anything from inside that folder.

Notes / decisions (spec was silent or the code differs):
  * capture uses --capture-scene 1 so the image is the rendered scene without the UI overlay; the
    baseline uses the same flags so the pixel comparison is apples-to-apples.
  * `mixed` kind effects are rendered with both --mme (post form) and --mme-model (object form) in
    one run.
  * static pass rate = mme_compile_test's passes_ok / passes_total for the primary entry point.
  * `--static-only`, `--no-static`, `--sheet-only` split the stages for the verification steps.
  * the results store is merged across runs (so a --static-only pass followed by an --only render
    pass keeps both halves).
"""

import argparse
import datetime
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------------------------

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent.parent
LIB_DIR = REPO_ROOT / "captures" / "mme_lib"
CATALOG = REPO_ROOT / "docs" / "mme" / "research" / "effects_catalog.json"
STATUS = LIB_DIR / "_status.json"
BUILD_DIR = REPO_ROOT / "build_mme"
BIN_DIR = BUILD_DIR / "bin"
PARSE_TEST = BIN_DIR / "mme_parse_test.exe"
COMPILE_TEST = BIN_DIR / "mme_compile_test.exe"
APP_EXE = BIN_DIR / "MMDX12.exe"
APP_LOG = BIN_DIR / "mmdx12.log"
APP_INI = BIN_DIR / "mmdx12.ini"
OUT_DIR = REPO_ROOT / "captures" / "mme_compat"
RESULTS = OUT_DIR / "results.json"
SHEET = OUT_DIR / "sheet.png"
COMPAT_MD = REPO_ROOT / "docs" / "mme" / "compat_list.md"
THUMB_DIR = REPO_ROOT / "docs" / "mme" / "compat"   # tracked thumbnails (captures/ is git-ignored)
OVERRIDES = REPO_ROOT / "docs" / "mme" / "compat_overrides.json"
BASELINE_PNG = OUT_DIR / "_baseline.png"
BASELINE_LOG = OUT_DIR / "_baseline.log"

# Pinned scene (mme_design.md §6 / spec): identical for every effect.
SCENE = {
    "character": "pda_miku/head",
    "stage": "none",
    "song": "Evergreen",
    "seek": "20",
    "frames": "60",
    "width": "1280",
    "height": "720",
}
RENDER_TIMEOUT = 180.0        # seconds per app run
COMPILE_CHUNK = 40            # files per mme_compile_test invocation (command-line length)

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"]', re.MULTILINE)
FX_EXT = ".fx"


def reconfigure_stdio():
    """Force UTF-8 stdio with errors='replace' so the harness runs under `python -I` (which ignores
    PYTHON* env vars) with any console / redirection: a cp949 console cannot encode the Japanese
    half-width kana in effect names / paths."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def oem_encoding():
    """The console / OEM code page of this machine (cp949 on Korean Windows)."""
    try:
        import ctypes
        return "cp%d" % ctypes.windll.kernel32.GetOEMCP()
    except Exception:
        try:
            import locale
            return locale.getpreferredencoding(False)
        except Exception:
            return "latin-1"


def decode_subprocess(raw):
    """Decode subprocess output bytes: try UTF-8, else the OEM code page; never assume non-None."""
    if raw is None:
        return ""
    if isinstance(raw, str):
        return raw
    try:
        return raw.decode("utf-8")
    except (UnicodeDecodeError, LookupError):
        pass
    enc = oem_encoding()
    try:
        return raw.decode(enc, "replace")
    except (UnicodeDecodeError, LookupError):
        return raw.decode("latin-1", "replace")


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------

def norm_name(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def norm_path(p):
    return os.path.normcase(os.path.abspath(str(p)))


def find_same_stem(fx_path, ext):
    """M5c: the same-stem `<ext>` file next to `fx_path` in the same folder (case-insensitive). None when absent."""
    fx = Path(fx_path)
    want = fx.stem.lower()
    try:
        for entry in os.scandir(fx.parent):
            if not entry.is_file():
                continue
            if Path(entry.name).suffix.lower() != ext:
                continue
            if Path(entry.name).stem.lower() == want:
                return Path(entry.path)
    except OSError:
        return None
    return None


def decode_bytes(raw):
    for enc in ("utf-8-sig", "utf-8", "cp932", "latin-1"):
        try:
            return raw.decode(enc)
        except (UnicodeDecodeError, LookupError):
            continue
    return raw.decode("latin-1", "replace")


def read_text(path):
    try:
        return decode_bytes(Path(path).read_bytes())
    except OSError:
        return ""


def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(n, i + 2)
            continue
        if c == '"':
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i])
                    i += 1
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def resolve_rel(base, rel):
    """Resolve `rel` against directory `base`, case-insensitively."""
    rel = rel.replace("\\", "/")
    if not rel:
        return None
    if len(rel) >= 2 and rel[1] == ":":
        p = Path(rel)
        return p if p.exists() else None
    cur = Path(base)
    for comp in rel.split("/"):
        if comp in ("", "."):
            continue
        if comp == "..":
            cur = cur.parent
            continue
        cand = cur / comp
        if cand.exists():
            cur = cand
            continue
        found = None
        try:
            for entry in os.scandir(cur):
                if entry.name.lower() == comp.lower():
                    found = Path(entry.path)
                    break
        except OSError:
            return None
        if found is None:
            return None
        cur = found
    return cur if cur.exists() and cur.is_file() else None


def run(cmd, timeout=None, cwd=None):
    """Run a subprocess: bytes in, decoded with errors='replace' (utf-8, then the OEM code page).
    Output is never assumed to be non-None."""
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, cwd=cwd)
        return proc.returncode, decode_subprocess(proc.stdout), False
    except subprocess.TimeoutExpired as e:
        return -1, decode_subprocess(e.stdout), True
    except OSError as e:
        return -1, str(e), False


# ---------------------------------------------------------------------------------------------
# Load status + catalog
# ---------------------------------------------------------------------------------------------

def load_entries():
    status = json.loads(STATUS.read_text(encoding="utf-8"))
    catalog = {}
    if CATALOG.is_file():
        for e in json.loads(CATALOG.read_text(encoding="utf-8")):
            catalog[e.get("id")] = e
    entries = []
    for e in status:
        if e.get("status") != "ok":
            continue
        cat = catalog.get(e.get("id"), {})
        entries.append({
            "rank": int(e["rank"]),
            "id": e["id"],
            "name": e.get("name", e["id"]),
            "kind_hint": cat.get("kind", ""),
            "category": cat.get("category", ""),
            "folder": LIB_DIR / ("%03d_%s" % (int(e["rank"]), e["id"])),
        })
    entries.sort(key=lambda x: x["rank"])
    return entries


def load_overrides():
    if not OVERRIDES.is_file():
        return {}
    try:
        return json.loads(OVERRIDES.read_text(encoding="utf-8"))
    except Exception as e:
        log("  ! cannot read %s: %s" % (OVERRIDES, e))
        return {}


def load_results():
    if not RESULTS.is_file():
        return {}
    try:
        data = json.loads(RESULTS.read_text(encoding="utf-8"))
        return data.get("entries", {}) if isinstance(data, dict) else {}
    except Exception as e:
        log("  ! cannot read %s: %s" % (RESULTS, e))
        return {}


# ---------------------------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------------------------

def find_src_root(folder):
    """The effect's source tree: <folder>/src if present, else the folder itself."""
    src = folder / "src"
    return src if src.is_dir() else folder


FX_EXTS = (".fx", ".fxm")


def collect_fx(root):
    """Every MME / MMM effect file: .fx (MME) and .fxm (MMM-only). Discovery prefers an MME entry point."""
    files = []
    for r, _dirs, names in os.walk(root):
        for nm in names:
            if nm.lower().endswith(FX_EXTS):
                files.append(Path(r) / nm)
    files.sort()
    return files


# MikuMikuMoving-only marker: the `.fxm` extension, an `MMM_`-prefixed *semantic* (`float x : MMM_Something;`),
# or a body whose techniques all live inside `#ifdef MIKUMIKUMOVING` (the MME preprocess drops them, so the parser
# reports zero techniques). MME does not support these, so the harness reports tier N instead of failed compiles.
# A bare `#ifdef MIKUMIKUMOVING` guard is NOT a marker: MME-compatible effects use it too, to hide MMM-only blocks.
MMM_EXT = ".fxm"
MMM_SEMANTIC_RE = re.compile(r':\s*MMM_[A-Za-z0-9_]+')


def is_mmm_file(path, parse_record=None):
    if Path(path).suffix.lower() == MMM_EXT:
        return True
    if MMM_SEMANTIC_RE.search(strip_comments(read_text(path))):
        return True
    # No techniques under the MME preprocess (they are all inside MIKUMIKUMOVING) and the file is not just an
    # artificial include: the parser's technique count is the evidence.
    if parse_record is not None and parse_record.get("techniques") == []:
        return True
    return False


def discover_entry_points(src_root, all_fx):
    """Entry points = .fx files no other .fx in the tree #includes (resolved case-insensitively)."""
    included = set()
    for f in all_fx:
        text = strip_comments(read_text(f))
        for m in INCLUDE_RE.finditer(text):
            rel = m.group(1).strip()
            if not rel:
                continue
            inc = resolve_rel(f.parent, rel)
            if inc is None:
                inc = resolve_rel(src_root, rel)
            if inc is not None and inc.suffix.lower() in FX_EXTS:
                included.add(norm_path(inc))
    return [f for f in all_fx if norm_path(f) not in included]


def classify_entry_points(src_root):
    """Run mme_parse_test --summary over the whole src tree; return path -> summary record."""
    if not PARSE_TEST.is_file():
        return {}
    _rc, out, _to = run([str(PARSE_TEST), str(src_root), "--quiet", "--summary"])
    info = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "file" in rec:
            info[norm_path(rec["file"])] = rec
    return info


def read_readme_text(entry):
    """Concatenated text of the entry's readme files (from source.json), for primary-entry naming."""
    readme_text = ""
    src = entry["folder"] / "source.json"
    if src.is_file():
        try:
            meta = json.loads(src.read_text(encoding="utf-8"))
            for r in meta.get("readme", []):
                p = entry["folder"] / r
                if p.is_file():
                    readme_text += "\n" + read_text(p)
        except Exception:
            pass
    return readme_text


def choose_primary(entry, src_root, entry_points, info, readme_text):
    """Primary = named like the entry folder / id / readme-mentioned, else first post, else first object."""
    # fix 3: prefer an MME entry point over an MMM-only one when both exist (an MMM file targets a host we do not
    # reproduce). Only fall back to an MMM primary when the effect has no MME entry point at all.
    mmm = {f: is_mmm_file(f, info.get(norm_path(f))) for f in entry_points}
    mme_points = [f for f in entry_points if not mmm[f]]
    if mme_points:
        entry_points = mme_points
    folder_name = src_root.name if src_root.name != "src" else entry["folder"].name
    # the extracted sub-folder under src (e.g. o_Tonemap_v0_7) is also a name hint
    subfolders = []
    try:
        subfolders = [d.name for d in src_root.iterdir() if d.is_dir()]
    except OSError:
        pass
    keys = [entry["id"], entry["name"], folder_name] + subfolders

    def named(f):
        stem = norm_name(f.stem)
        if len(stem) < 4:
            return False
        for k in keys:
            kn = norm_name(k)
            if len(kn) >= 4 and (kn == stem or kn.startswith(stem) or stem.startswith(kn)):
                return True
        if f.name.lower() in readme_text.lower() or f.stem.lower() in readme_text.lower():
            return True
        return False

    def kind_of(f):
        return info.get(norm_path(f), {}).get("kind", "")

    named_files = [f for f in entry_points if named(f)]
    pool = named_files if named_files else entry_points
    # prefer an exact id/folder match, then a post-kind file, then anything
    def score(f):
        stem = norm_name(f.stem)
        exact = 0 if stem == norm_name(folder_name) or stem == norm_name(entry["id"]) else 1
        k = kind_of(f)
        krank = 0 if k == "post" else (1 if k == "object" else 2)
        return (exact, krank, str(f).lower())
    if named_files:
        pool.sort(key=score)
    return pool[0]


# ---------------------------------------------------------------------------------------------
# Static stage
# ---------------------------------------------------------------------------------------------

def static_compile(files, object_mode):
    """Compile `files` (chunked) with mme_compile_test; return path -> {ok,passes_ok,passes_total,first_error}."""
    records = {}
    for i in range(0, len(files), COMPILE_CHUNK):
        chunk = files[i:i + COMPILE_CHUNK]
        json_out = OUT_DIR / ("_static_%s_%d.json" % ("obj" if object_mode else "post", i))
        cmd = [str(COMPILE_TEST)] + [str(f) for f in chunk] + ["--no-selftest", "--json", str(json_out)]
        if object_mode:
            cmd.append("--object-mode")
        run(cmd, timeout=600, cwd=str(BIN_DIR))
        if json_out.is_file():
            try:
                for rec in json.loads(json_out.read_text(encoding="utf-8")):
                    records[norm_path(rec.get("file", ""))] = rec
            except Exception:
                pass
            json_out.unlink(missing_ok=True)
    return records


# ---------------------------------------------------------------------------------------------
# Render stage
# ---------------------------------------------------------------------------------------------

def wait_for_no_app(max_seconds=300.0, poll=5.0):
    """Block until no MMDX12.exe process is running (one window at a time). A failed tasklist is
    treated as 'unknown': wait 2 s and retry for up to 30 s, then continue anyway."""
    deadline = time.time() + max_seconds
    unknown_until = time.time() + 30.0
    while True:
        try:
            proc = subprocess.run(["tasklist", "/FI", "IMAGENAME eq MMDX12.exe", "/NH"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            out = decode_subprocess(proc.stdout)
        except Exception:
            out = None
        if out is None or not out.strip():
            # tasklist failed / gave nothing: unknown state, retry briefly, then give up waiting.
            if time.time() > unknown_until:
                return
            log("  tasklist returned nothing; retrying ...")
            time.sleep(2.0)
            continue
        if "MMDX12.exe" not in out:
            return
        if time.time() > deadline:
            raise SystemExit("a MMDX12.exe process is still running after %.0f s; close it and retry" % max_seconds)
        log("  waiting for the running MMDX12.exe to exit ...")
        time.sleep(poll)


def scene_args(capture_png):
    return [
        "--character", SCENE["character"], "--stage", SCENE["stage"], "--song", SCENE["song"],
        "--autoplay", "--seek", SCENE["seek"], "--paused", "--no-physics", "--frames", SCENE["frames"],
        "--effect", "none", "--shader-pack", "none", "--dof", "0", "--render", "raster",
        "--upscaler", "none", "--width", SCENE["width"], "--height", SCENE["height"],
        "--capture", str(capture_png), "--capture-scene", "1",
    ]


def run_app(extra, capture_png, log_copy):
    """One sequential app run with the pinned scene + `extra`; copy the log. Returns signal dict."""
    wait_for_no_app()
    cmd = [str(APP_EXE)] + scene_args(capture_png) + list(extra)
    log("  $ " + " ".join('"%s"' % c if " " in str(c) else str(c) for c in cmd))
    t0 = time.time()
    exit_code, _out, timed_out = run(cmd, timeout=RENDER_TIMEOUT, cwd=str(BIN_DIR))
    elapsed = time.time() - t0
    text = APP_LOG.read_text(encoding="utf-8", errors="replace") if APP_LOG.is_file() else ""
    if log_copy is not None:
        try:
            shutil.copy2(APP_LOG, log_copy)
        except OSError:
            pass
    return {"exit_code": exit_code, "timeout": timed_out, "elapsed_s": round(elapsed, 1),
            "log": text}


def parse_signals(sig):
    text = sig.get("log", "")
    lines = text.splitlines()
    errors = [l.strip() for l in lines if "[E]" in l]
    warnings = [l.strip() for l in lines if "[W]" in l and " unknown command line argument" not in l]
    crash = any("CRASH" in l for l in errors)
    programs = []
    for m in re.finditer(r"MME LOAD .*? programs=(\d+)/(\d+)", text):
        programs.append({"ok": int(m.group(1)), "total": int(m.group(2))})
    drew = ("MME FRAME" in text) or ("MME OBJECT" in text)
    reached = ("motion bound" in text) or ("STARTUP first frame" in text)
    return {
        "exit_code": sig.get("exit_code"),
        "timeout": sig.get("timeout", False),
        "elapsed_s": sig.get("elapsed_s"),
        "crash": crash or (sig.get("exit_code") not in (0, None)),
        "errors": errors,
        "warnings": warnings,
        "programs": programs,
        "drew": drew,
        "reached_scene": reached,
        "loaded": bool(programs),
    }


# ---------------------------------------------------------------------------------------------
# Image metrics
# ---------------------------------------------------------------------------------------------

def background_estimate(np, rgb):
    """Per-row background of a frame: the character is a compact, coloured silhouette centred in the
    frame, so the median colour of each row is the sky / floor behind it (the backdrop gradient is smooth
    horizontally). Shape (h, 1, 3), broadcastable over x."""
    return np.median(rgb, axis=1, keepdims=True)


def image_metrics(baseline_png, effect_png):
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        return {"error": "image metrics need numpy+Pillow: %s" % e}
    if not Path(baseline_png).is_file() or not Path(effect_png).is_file():
        return {"error": "missing image"}
    try:
        a = np.asarray(Image.open(baseline_png).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(effect_png).convert("RGB"), dtype=np.int16)
    except Exception as e:
        return {"error": "cannot open image: %s" % e}
    if a.shape != b.shape:
        return {"error": "size mismatch %s vs %s" % (a.shape, b.shape)}
    diff = np.abs(a - b)
    mean_abs = float(diff.mean())
    changed = float((diff.max(axis=2) > 2).mean()) * 100.0
    gray = np.asarray(Image.open(effect_png).convert("L"), dtype=np.float32)
    flat = float(gray.std())
    effect_mean = float(b.mean())
    # "NaN-ish" detection: a PNG is 8-bit clamped, so a failed / zeroed readback shows up as an
    # all-zero alpha channel or a frame with no spatial spread.
    alpha = np.asarray(Image.open(effect_png).convert("RGBA"), dtype=np.uint8)[..., 3]
    alpha_zero = float((alpha == 0).mean())
    nan_ish = (flat < 0.5) or (alpha_zero > 0.99)
    # "character region vanished" (fix 3): the baseline's character mask = pixels that stand out from the
    # frame's own background (per-row median). If the effect frame paints (almost) all of those pixels back
    # to the background, the character disappeared behind / into the backdrop, even though colour draws ran.
    bg = background_estimate(np, a)
    char = np.abs(a - bg).max(axis=2) > 28
    char_px = int(char.sum())
    vanished = 0.0
    char_vanished = False
    if char_px > 500:
        # "painted back to the background": the effect pixel is close to the baseline's background there.
        back_to_bg = np.abs(b - bg).max(axis=2) <= 18
        vanished = float((char & back_to_bg).sum()) / float(char_px)
        char_vanished = vanished > 0.75
    # "one colour frame" (fix 3): almost every pixel the same colour (a fill / a stuck target), e.g. a glow that
    # overwrote the whole frame. Measured on a 4-level quantisation so smooth gradients do not count.
    q = (b // 32).astype(np.int32)
    keys = (q[..., 0] << 8) | (q[..., 1] << 4) | q[..., 2]
    _, counts = np.unique(keys, return_counts=True)
    one_colour_pct = float(counts.max()) / float(keys.size) * 100.0
    one_colour = one_colour_pct > 92.0
    return {
        "mean_abs_diff": round(mean_abs, 4),
        "pct_pixels_changed": round(changed, 3),
        "effect_mean_luma": round(effect_mean, 3),
        "effect_std": round(flat, 3),
        "alpha_zero_pct": round(alpha_zero * 100.0, 3),
        "fully_black": effect_mean < 1.0,
        "fully_white": effect_mean > 254.0,
        "flat_frame": flat < 0.5,
        "nan_ish": nan_ish,
        "char_pixels": char_px,
        "char_vanished_pct": round(vanished * 100.0, 1),
        "char_vanished": char_vanished,
        "one_colour_pct": round(one_colour_pct, 1),
        "one_colour": one_colour,
    }


# ---------------------------------------------------------------------------------------------
# Tier guess
# ---------------------------------------------------------------------------------------------

def tier_guess(static, render):
    """X = crash / no LOAD / 0 programs; C = loaded but did not draw or black/white or <50% programs;
    B = drew with warnings or partial programs; A = drew, all programs, no [E]."""
    if render is None or not render.get("ran"):
        return ""
    if render.get("crash") or render.get("timeout") or not render.get("loaded"):
        return "X"
    progs = render.get("programs") or []
    total = sum(p["total"] for p in progs)
    ok = sum(p["ok"] for p in progs)
    if total == 0:
        return "X"
    metrics = render.get("metrics") or {}
    all_programs = ok == total
    partial = 0 < ok < total
    drew = render.get("drew", False)
    has_error = bool(render.get("errors"))
    black_white = (metrics.get("fully_black") or metrics.get("fully_white") or metrics.get("flat_frame")
                   or metrics.get("nan_ish"))
    # fix 3: a frame whose character region was painted back into the backdrop (the character vanished), or that is
    # one flat colour, is at most C, even when the program count is fine.
    if (not drew or black_white or metrics.get("char_vanished") or metrics.get("one_colour")
            or (total and ok * 2 < total)):
        return "C"
    if render.get("warnings") or partial:
        return "B"
    if drew and all_programs and not has_error:
        return "A"
    return "B"


# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------

def render_contact_sheet(entries, store, overrides):
    try:
        from PIL import Image, ImageDraw
    except Exception as e:
        log("  ! contact sheet needs Pillow: %s" % e)
        return
    # the sheet shows what actually rendered (thumbnails with name + tier); the full report incl.
    # the "not run" rows is docs/mme/compat_list.md.
    tiles = []
    for e in entries:
        rid = str(e["rank"])
        rec = store.get(rid, {})
        png = OUT_DIR / ("%s_%s.png" % (e["rank"], e["id"]))
        if not png.is_file():
            continue
        tier = e.get("_final_tier") or rec.get("tier") or "?"
        tiles.append((e["name"], tier, png))
    if not tiles:
        log("  contact sheet: no rendered captures yet")
        return

    cols = min(4, len(tiles))
    tw, th = 320, 180
    header = 26
    pad = 6
    rows = max(1, math.ceil(len(tiles) / cols))
    cell_w = tw + pad * 2
    cell_h = th + header + pad * 2
    sheet = Image.new("RGB", (cols * cell_w, rows * cell_h), (24, 24, 28))
    draw = ImageDraw.Draw(sheet)
    font = None
    for name in ("Pretendard-Bold.otf", "Pretendard-SemiBold.otf"):
        p = REPO_ROOT / "assets" / "fonts" / name
        if p.is_file():
            try:
                from PIL import ImageFont
                font = ImageFont.truetype(str(p), 15)
                break
            except Exception:
                font = None
    for i, (name, tier, png) in enumerate(tiles):
        cx = (i % cols) * cell_w
        cy = (i // cols) * cell_h
        try:
            img = Image.open(png).convert("RGB").resize((tw, th), Image.Resampling.LANCZOS)
            sheet.paste(img, (cx + pad, cy + pad + header))
        except Exception:
            draw.rectangle([cx + pad, cy + pad + header, cx + pad + tw, cy + pad + header + th],
                           fill=(40, 40, 46))
        label = "%s  [%s]" % (name[:38], tier)
        color = {"A": (120, 230, 120), "B": (240, 230, 120), "C": (240, 160, 100), "X": (240, 110, 110)}.get(
            tier, (200, 200, 210))
        draw.text((cx + pad, cy + pad + 4), label, fill=color, font=font)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    sheet.save(SHEET, format="PNG")
    log("  contact sheet: %s" % SHEET)


def write_report(entries, store, overrides, baseline_ok):
    lines = []
    lines.append("# MME compatibility list")
    lines.append("")
    lines.append("Generated by `tools/mme/mme_compat_run.py` (M7a harness). Tier rules: `docs/mme/mme_design.md` §4.")
    lines.append("")
    lines.append("Auto tiers are a first guess; the architect sets the final tier in "
                 "`docs/mme/compat_overrides.json` (`{\"<rank>\": {\"final_tier\": \"A\", \"notes\": \"...\"}}`).")
    lines.append("")
    lines.append("Static = `mme_compile_test` pass rate of the primary entry point. Render = the pinned scene "
                 "(`%s` / `%s` / `%s` @ %ss) with `--mme-object` (an accessory `.x` + its same-stem `.fx`), `--mme` "
                 "(post) or `--mme-model` (an object shader with no `.x`)." %
                 (SCENE["character"], SCENE["stage"], SCENE["song"], SCENE["seek"]))
    lines.append("")
    lines.append("| rank | name | kind | static | drew | image delta | tier | final | notes | capture |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for e in entries:
        rid = str(e["rank"])
        rec = store.get(rid, {})
        disc = rec.get("discovery") or {}
        static = rec.get("static") or {}
        render = rec.get("render") or {}
        ov = overrides.get(rid) or overrides.get(str(e["id"])) or {}
        kind = disc.get("primary_kind") or e.get("kind_hint") or ""
        skipped = rec.get("skipped") or e.get("_skipped")
        mmm_only = disc["mmm"] if "mmm" in disc else rec.get("mmm_only")
        if mmm_only:
            static_cell = "—"
        elif skipped:
            static_cell = "—"
        elif static.get("passes_total") is not None:
            total = static.get("passes_total") or 0
            ok = static.get("passes_ok") or 0
            static_cell = "%d/%d" % (ok, total)
        else:
            static_cell = "not run"
        if mmm_only:
            drew = "not run"
            delta = "not run"
        elif render.get("ran"):
            drew = "yes" if render.get("drew") else "no"
            m = render.get("metrics") or {}
            delta = ("%.2f / %.1f%%" % (m.get("mean_abs_diff", 0.0), m.get("pct_pixels_changed", 0.0))
                     if "mean_abs_diff" in m else "?")
        else:
            drew = "not run"
            delta = "not run"
        tier = rec.get("tier") or ""
        final = ov.get("final_tier", "") or ("N" if mmm_only else "")
        notes = ov.get("notes", "")
        if mmm_only:
            notes = (notes + " " if notes else "") + "MMM-only (tier N)"
        png = OUT_DIR / ("%s_%s.png" % (e["rank"], e["id"]))
        link = "—"
        if png.is_file():
            link = "[jpg](compat/%s_%s.jpg)" % (e["rank"], e["id"])
            publish_thumb(png, THUMB_DIR / ("%s_%s.jpg" % (e["rank"], e["id"])))
        name = e["name"]
        if skipped:
            notes = (notes + " " if notes else "") + skipped
        lines.append("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s |" %
                     (e["rank"], name.replace("|", "\\|"), kind, static_cell, drew, delta,
                      tier, final, notes.replace("|", "\\|"), link))
    lines.append("")
    COMPAT_MD.parent.mkdir(parents=True, exist_ok=True)
    COMPAT_MD.write_text("\n".join(lines), encoding="utf-8")
    log("  report: %s" % COMPAT_MD)


def publish_thumb(src, dst):
    """Downscaled JPEG copy of a capture for the tracked docs (the full PNGs stay in captures/)."""
    try:
        from PIL import Image
    except ImportError:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src) as im:
        im = im.convert("RGB")
        im.thumbnail((640, 360))
        im.save(dst, "JPEG", quality=82, optimize=True)


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------

def select_entries(entries, only, limit):
    if only:
        want = set(x.strip().lower() for x in only.split(",") if x.strip())
        out = [e for e in entries if str(e["rank"]) in want or e["id"].lower() in want]
        missing = want - set([str(e["rank"]) for e in out]) - set([e["id"].lower() for e in out])
        if missing:
            log("  ! --only entries not found: %s" % ", ".join(sorted(missing)))
    else:
        out = list(entries)
    if limit and limit > 0:
        out = out[:limit]
    return out


def discover_all(entries):
    """Fill each entry's discovery info (entry points + primary). Returns (primary files, meta)."""
    meta = {}
    for e in entries:
        if not e["folder"].is_dir():
            e["_skipped"] = "no folder"
            continue
        src_root = find_src_root(e["folder"])
        all_fx = collect_fx(src_root)
        if not all_fx:
            e["_skipped"] = "no .fx files"
            continue
        info = classify_entry_points(src_root)
        eps = discover_entry_points(src_root, all_fx)
        if not eps:
            eps = all_fx   # include cycle: fall back to every file
        readme_text = read_readme_text(e)
        primary = choose_primary(e, src_root, eps, info, readme_text)
        variants = sorted([f for f in eps if f != primary])
        e["_primary"] = primary
        e["_variants"] = variants
        e["_entry_kinds"] = {str(f): info.get(norm_path(f), {}).get("kind", "") for f in eps}
        pkind = info.get(norm_path(primary), {}).get("kind", "")
        techniques = info.get(norm_path(primary), {}).get("techniques", [])
        e["_primary_kind"] = pkind
        e["_mmm"] = is_mmm_file(primary, info.get(norm_path(primary)))
        e["_offscreen"] = info.get(norm_path(primary), {}).get("offscreen", False)
        meta[e["rank"]] = {
            "entry_points": len(eps),
            "variants": len(variants),
            "primary": str(primary.relative_to(REPO_ROOT)).replace("\\", "/"),
            "primary_kind": pkind,
            "primary_techniques": techniques,
            "offscreen": info.get(norm_path(primary), {}).get("offscreen", False),
            "mmm": e["_mmm"],
        }
        log("  #%d %s: %d entry points (kind=%s%s) primary=%s" %
            (e["rank"], e["id"], len(eps), pkind or "?", ", MMM" if e["_mmm"] else "", primary.name))
    return meta


def main():
    reconfigure_stdio()
    ap = argparse.ArgumentParser(description="M7a MME compatibility harness")
    ap.add_argument("--only", default="", help="comma-separated ranks or ids to run (default: all)")
    ap.add_argument("--resume", action="store_true", help="skip entries that already have a render result")
    ap.add_argument("--limit", type=int, default=0, help="only the first N entries")
    ap.add_argument("--all-variants", action="store_true", help="also render the non-primary entry points")
    ap.add_argument("--static-only", action="store_true", help="static stage only; no app windows")
    ap.add_argument("--no-static", action="store_true", help="skip the static stage (render only)")
    ap.add_argument("--sheet-only", action="store_true", help="regenerate the report/sheet from results.json; no runs")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if not (PARSE_TEST.is_file() and COMPILE_TEST.is_file() and APP_EXE.is_file()):
        log("missing build_mme binaries (mme_parse_test.exe / mme_compile_test.exe / MMDX12.exe)")
        return 2

    all_entries = load_entries()
    overrides = load_overrides()
    store = load_results()
    reconstruct_missing(all_entries, store)   # recover render data lost by an earlier crashed run
    selected = select_entries(all_entries, args.only, args.limit)

    log("MME compatibility harness: %d ok entries, %d selected" % (len(all_entries), len(selected)))

    if args.sheet_only:
        for e in all_entries:
            e["_final_tier"] = (overrides.get(str(e["rank"])) or overrides.get(e["id"]) or {}).get("final_tier", "")
        reconstruct_missing(all_entries, store)
        write_report(all_entries, store, overrides, (OUT_DIR / "_baseline.png").is_file())
        render_contact_sheet(all_entries, store, overrides)
        write_results(store)
        log_summary(all_entries, store)
        return 0

    # ---- discovery ----
    log("-- discover --")
    meta = discover_all(selected)
    for e in selected:
        rid = str(e["rank"])
        rec = store.setdefault(rid, {"rank": e["rank"], "id": e["id"], "name": e["name"]})
        rec["rank"] = e["rank"]
        rec["id"] = e["id"]
        rec["name"] = e["name"]
        rec["kind_hint"] = e.get("kind_hint", "")
        rec["skipped"] = e.get("_skipped")
        # fix 3: recomputed each run so a stale flag from an earlier (buggy) classification does not linger.
        rec["mmm_only"] = bool(e.get("_mmm"))
        if e["rank"] in meta:
            rec["discovery"] = meta[e["rank"]]

    # ---- static stage ----
    if not args.no_static:
        log("-- static --")
        post_files, obj_files = [], []
        for e in selected:
            if e.get("_primary") is None:
                continue
            if e.get("_mmm"):
                continue   # fix 3: MMM-only (tier N): no static compile, no render
            files = [e["_primary"]] + (e["_variants"] if args.all_variants else [])
            kinds = e.get("_entry_kinds", {})
            for f in files:
                if f == e["_primary"]:
                    k = e["_primary_kind"]
                else:
                    k = kinds.get(str(f), e["_primary_kind"])
                (obj_files if k == "object" else post_files).append((e, f))
        post_recs = static_compile([f for _e, f in post_files], object_mode=False) if post_files else {}
        obj_recs = static_compile([f for _e, f in obj_files], object_mode=True) if obj_files else {}
        recs = {**post_recs, **obj_recs}
        by_rank = {}
        for e, f in post_files + obj_files:
            by_rank.setdefault(e["rank"], []).append((f, recs.get(norm_path(f))))
        for e in selected:
            if e.get("_primary") is None:
                continue
            rid = str(e["rank"])
            try:
                if e.get("_mmm"):
                    # fix 3: an MMM-only effect (tier N) is skipped by design, never a failed compile.
                    store[rid]["static"] = None
                    store[rid]["mmm_only"] = True
                    log("  #%d %s static skipped (MMM-only, tier N)" % (e["rank"], e["id"]))
                    continue
                rows = by_rank.get(e["rank"], [])
                prim = next(((f, r) for f, r in rows if f == e["_primary"]), None)
                def pack(r):
                    if not r:
                        return None
                    return {"ok": r.get("ok"), "passes_ok": r.get("passes_ok"), "passes_total": r.get("passes_total"),
                            "first_error": r.get("first_error", "")}
                static = pack(prim[1]) if prim else None
                if static:
                    tot = static.get("passes_total") or 0
                    static["pass_rate"] = round((static.get("passes_ok") or 0) / tot, 3) if tot else 1.0
                store[rid]["static"] = static
                if args.all_variants:
                    store[rid]["static_variants"] = [
                        {"file": str(f.relative_to(REPO_ROOT)).replace("\\", "/"), **(pack(r) or {})}
                        for f, r in rows if f != e["_primary"]]
                okc = static.get("passes_ok") if static else None
                totc = static.get("passes_total") if static else None
                log("  #%d %s static %s/%s" % (e["rank"], e["id"], okc, totc))
            except Exception as exc:
                record_harness_error(store, rid, e, exc)
            write_results(store)   # persist after every entry so a later crash loses nothing

    # ---- render stage ----
    if not args.static_only:
        log("-- render --")
        need_baseline = not BASELINE_PNG.is_file()
        if need_baseline:
            log("  baseline capture (no MME)")
            sig = run_app([], BASELINE_PNG, BASELINE_LOG)
            base = parse_signals(sig)
            if not base.get("reached_scene"):
                log("  ! baseline never reached a scene; aborting the render stage")
                write_results(store)
                return 3
        else:
            log("  baseline reused: %s" % BASELINE_PNG)

        ini_backup = APP_INI.with_suffix(".ini.mmecompat.bak")
        have_ini = APP_INI.is_file()
        if have_ini:
            shutil.copy2(APP_INI, ini_backup)
        try:
            for e in selected:
                rid = str(e["rank"])
                if e.get("_primary") is None:
                    continue
                if e.get("_mmm"):
                    continue   # fix 3: MMM-only effect (tier N): never rendered
                rec = store[rid]
                if args.resume and (rec.get("render") or {}).get("ran"):
                    log("  #%d %s: resume, already rendered" % (e["rank"], e["id"]))
                    continue
                try:
                    png = OUT_DIR / ("%s_%s.png" % (e["rank"], e["id"]))
                    log_copy = OUT_DIR / ("%s_%s.log" % (e["rank"], e["id"]))
                    kind = e["_primary_kind"] or "post"
                    extra = []
                    # M5c: an effect object (accessory `.x` next to the primary `.fx`) is loaded the way MMD users
                    # do: `--mme-object <the .x>` (its same-stem `.fx` is assigned to it). Pure model shaders with no
                    # `.x` keep `--mme-model`.
                    xfile = find_same_stem(e["_primary"], ".x")
                    if xfile is not None:
                        extra = ["--mme-object", str(xfile)]
                        mode = "object"
                    elif kind == "post":
                        extra = ["--mme", str(e["_primary"])]
                        mode = "post"
                    elif kind == "object":
                        extra = ["--mme-model", str(e["_primary"])]
                        mode = "model"
                    else:  # mixed: both forms
                        extra = ["--mme", str(e["_primary"]), "--mme-model", str(e["_primary"])]
                        mode = "both"
                    log("  #%d %s (%s, mode=%s)" % (e["rank"], e["id"], kind, mode))
                    sig = run_app(extra, png, log_copy)
                    render = parse_signals(sig)
                    render["ran"] = True
                    render["kind_used"] = kind
                    render["mode_used"] = mode
                    render["fx"] = str(e["_primary"].relative_to(REPO_ROOT)).replace("\\", "/")
                    if xfile is not None:
                        render["x_used"] = str(xfile.relative_to(REPO_ROOT)).replace("\\", "/")
                    render["metrics"] = image_metrics(BASELINE_PNG, png) if png.is_file() else {"error": "no capture"}
                    render["tier"] = tier_guess(rec.get("static"), render)
                    rec["render"] = render
                    rec["tier"] = render["tier"]
                    log("    exit=%s drew=%s programs=%s tier=%s metrics=%s" %
                        (render["exit_code"], render["drew"],
                         ",".join("%d/%d" % (p["ok"], p["total"]) for p in render["programs"]) or "-",
                         render["tier"], render["metrics"].get("mean_abs_diff", render["metrics"].get("error", "?"))))

                    if args.all_variants:
                        for v in e["_variants"]:
                            vkind = e.get("_entry_kinds", {}).get(str(v), "") or "post"
                            vpng = OUT_DIR / ("%s_%s__%s.png" % (e["rank"], e["id"], v.stem))
                            vlog = OUT_DIR / ("%s_%s__%s.log" % (e["rank"], e["id"], v.stem))
                            vflag = "--mme-model" if vkind == "object" else "--mme"
                            vsig = run_app([vflag, str(v)], vpng, vlog)
                            vrender = parse_signals(vsig)
                            vrender["ran"] = True
                            vrender["kind_used"] = vkind
                            vrender["fx"] = str(v.relative_to(REPO_ROOT)).replace("\\", "/")
                            vrender["metrics"] = image_metrics(BASELINE_PNG, vpng) if vpng.is_file() else {"error": "no capture"}
                            vrender["tier"] = tier_guess(None, vrender)
                            rec.setdefault("render_variants", []).append(vrender)
                            log("    variant %s drew=%s tier=%s" % (v.name, vrender["drew"], vrender["tier"]))
                except Exception as exc:
                    record_harness_error(store, rid, e, exc)
                write_results(store)   # persist after every entry so a later crash loses nothing
        finally:
            if have_ini and ini_backup.is_file():
                shutil.copy2(ini_backup, APP_INI)
                ini_backup.unlink(missing_ok=True)

    # ---- report over ALL ok entries ----
    log("-- report --")
    reconstruct_missing(all_entries, store)
    for e in all_entries:
        e["_final_tier"] = (overrides.get(str(e["rank"])) or overrides.get(e["id"]) or {}).get("final_tier", "")
    write_report(all_entries, store, overrides, BASELINE_PNG.is_file())
    render_contact_sheet(all_entries, store, overrides)
    write_results(store)
    log_summary(all_entries, store)
    return 0


def write_results(store):
    doc = {
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "scene": SCENE,
        "baseline": str(BASELINE_PNG.relative_to(REPO_ROOT)).replace("\\", "/") if BASELINE_PNG.is_file() else None,
        "entries": store,
    }
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tmp = RESULTS.with_name(RESULTS.name + ".tmp")
    tmp.write_text(json.dumps(doc, indent=1, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, RESULTS)
    log("  results: %s" % RESULTS)


def reconstruct_render(e, rec):
    """Rebuild an entry's render data from its existing PNG + log (an earlier run whose result was
    dropped from results.json). Returns the render dict, or None when the pair is not both present."""
    png = OUT_DIR / ("%s_%s.png" % (e["rank"], e["id"]))
    log_path = OUT_DIR / ("%s_%s.log" % (e["rank"], e["id"]))
    if not png.is_file() or not log_path.is_file():
        return None
    text = read_text(log_path)
    sig = {"exit_code": None, "timeout": False, "elapsed_s": None, "log": text}
    render = parse_signals(sig)
    render["ran"] = True
    disc = rec.get("discovery") or {}
    render["kind_used"] = disc.get("primary_kind") or rec.get("kind_hint") or ""
    render["fx"] = disc.get("primary") or ""
    render["reconstructed"] = True
    render["metrics"] = image_metrics(BASELINE_PNG, png) if png.is_file() else {"error": "no capture"}
    render["tier"] = tier_guess(rec.get("static"), render)
    return render


def reconstruct_missing(entries, store):
    """Fill in render data for entries whose PNG + log exist on disk but are missing from the store
    (so results lost by an earlier crashed run are recovered)."""
    n = 0
    for e in entries:
        rid = str(e["rank"])
        rec = store.get(rid)
        if rec is not None and (rec.get("render") or {}).get("ran"):
            continue
        if rec is not None and rec.get("mmm_only"):
            continue
        if rec is None:
            rec = {"rank": e["rank"], "id": e["id"], "name": e["name"], "kind_hint": e.get("kind_hint", "")}
        render = reconstruct_render(e, rec)
        if render is None:
            continue
        rec["render"] = render
        rec["tier"] = render["tier"]
        store[rid] = rec
        n += 1
    if n:
        log("  reconstructed render data for %d entr%s from existing PNG+logs" % (n, "y" if n == 1 else "ies"))


def record_harness_error(store, rid, e, exc):
    """Record a harness error for one entry (tier X) so the batch continues past it."""
    msg = "harness error: %s: %s" % (type(exc).__name__, exc)
    rec = store.setdefault(rid, {})
    rec.setdefault("rank", e["rank"])
    rec.setdefault("id", e["id"])
    rec.setdefault("name", e["name"])
    render = rec.get("render") or {}
    render["ran"] = True
    render["errors"] = list(render.get("errors") or []) + [msg]
    render["harness_error"] = msg
    rec["render"] = render
    rec["tier"] = "X"
    log("  #%d %s HARNESS ERROR: %s" % (e["rank"], e["id"], msg))


def log_summary(entries, store):
    counts = {}
    for e in entries:
        rec = store.get(str(e["rank"]), {})
        if rec.get("mmm_only") or e.get("_mmm"):
            tier = "N"
        else:
            tier = rec.get("tier") or "not-run"
        if not tier:
            tier = "not-run"
        counts[tier] = counts.get(tier, 0) + 1
    order = ["A", "B", "C", "X", "N", "not-run"]
    parts = ["%s=%d" % (t, counts[t]) for t in order if t in counts]
    parts += ["%s=%d" % (t, counts[t]) for t in sorted(counts) if t not in order]
    log("summary: %s (total %d)" % (", ".join(parts), len(entries)))


if __name__ == "__main__":
    sys.exit(main())
