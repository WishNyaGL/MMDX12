#!/usr/bin/env python3
"""Regenerate the README / promo comparison media for MMDX12 (work unit P1.1).

One command renders the SAME frame of the SAME scene as
  classic     -- vanilla MMD look (--classic: no SSAO/SSR/bloom/volumetrics/DoF/LUT/effects/soft shadows)
  raster      -- MMDX12 raster
  rt          -- MMDX12 real-time ray tracing
  pt          -- MMDX12 real-time path tracing
  offline_gi  -- offline GI still (path-traced, high quality)
and composes them into labelled side-by-side (1x4) / 2x2 grids and a hero split image
(left half classic, right half PT). Every capture is pinned (--seek --paused --no-physics --frames,
fixed width/height, --lighting, --dof, --render) and runs are strictly sequential, each wrapped in a
timeout and checked against bin/mmdx12.log. bin/mmdx12.ini is backed up and restored around the batch.

Composition uses Pillow when available, else ffmpeg, else the raw PNGs are left in place (see the
warning printed at the end). Output names are stable so README links never change.
"""

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

if sys.stdout:
    sys.stdout.reconfigure(encoding="utf-8")
if sys.stderr:
    sys.stderr.reconfigure(encoding="utf-8")

REPO_ROOT = Path(__file__).resolve().parent.parent

# Scene presets: character / stage / song substrings (name matching = first case-insensitive substring hit),
# seek seconds, lighting preset (app/Lighting.h: 0 Studio, 1 Sunset, 2 Concert, 3 Night), DoF (0/1), and an
# optional fixed free camera "--camera tx,ty,tz,yawDeg,pitchDeg,dist" (None = the motion camera at `seek`).
SCENE_PRESETS = {
    "default": {
        "character": "01_9001",
        "stage": "terrace",
        "song": "Evergreen",
        "seek": 47.0,   # face visible, turned toward the camera (20 s: a hand over the face)
        "lighting": 0,
        "dof": 0,
        "camera": None,
    },
    "concert": {
        "character": "sour_miku",
        "stage": "theater",
        "song": "catch_the_wave",
        "seek": 12.0,
        "lighting": 2,
        "dof": 0,
        "camera": None,
    },
}

# Variant order (the labels are the panel captions; keep the text stable for README links).
VARIANTS = ["classic", "raster", "rt", "pt", "offline_gi"]
LABELS = {
    "classic": "MMD classic",
    "raster": "MMDX12 Raster",
    "rt": "MMDX12 RT",
    "pt": "MMDX12 Path tracing",
    "offline_gi": "Offline GI",
}
# The four primary looks used in the 2x2 grid and the 1x4 strip (offline GI stays a standalone still).
GRID_VARIANTS = ["classic", "raster", "rt", "pt"]
SPLIT_RIGHT = "raster"   # right half of compare_split (--split-right)
# Offline video renderer per variant for --clips (classic rides the offline raster path with --classic).
CLIP_RENDERERS = {"classic": "raster", "raster": "raster", "rt": "rt", "pt": "pt"}


def find_exe(explicit: str) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            raise SystemExit(f"MMDX12.exe not found: {p}")
        return p
    for cand in ("build", "build_dev"):
        p = REPO_ROOT / cand / "bin" / "MMDX12.exe"
        if p.is_file():
            return p
    raise SystemExit("MMDX12.exe not found; pass --exe (e.g. --exe build_c/bin/MMDX12.exe)")


def wait_for_no_app(max_seconds: float = 300.0, poll: float = 10.0) -> None:
    """Block until no MMDX12.exe process is running (runs open a window; only one at a time)."""
    deadline = time.time() + max_seconds
    while True:
        try:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq MMDX12.exe", "/NH"],
                                 capture_output=True, text=True, timeout=30).stdout
        except Exception:
            return  # tasklist unavailable: do not block the run
        if "MMDX12.exe" not in out:
            return
        if time.time() > deadline:
            raise SystemExit("a MMDX12.exe process is still running after 5 minutes; close it and retry")
        print("  waiting for the running MMDX12.exe to exit ...")
        time.sleep(poll)


def run_capture(cmd: list, log_path: Path, timeout: float, dry_run: bool) -> bool:
    """Run one app invocation; True when it reached a scene and logged no [E] lines."""
    printable = " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd)
    print(f"  $ {printable}")
    if dry_run:
        return True
    wait_for_no_app()
    if log_path.is_file():
        try:
            log_path.unlink()   # ensure the check reads THIS run's log
        except OSError:
            pass
    try:
        proc = subprocess.run(cmd, timeout=timeout, cwd=str(Path(cmd[0]).parent))
    except subprocess.TimeoutExpired:
        print(f"  ! timed out after {timeout:.0f}s")
        return False
    text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else ""
    reached = ("motion bound" in text) or ("STARTUP first frame" in text)
    errors = [l for l in text.splitlines() if "[E]" in l]
    if not reached:
        print("  ! run never reached a scene (no 'motion bound' / 'STARTUP first frame' in the log)")
        return False
    if errors:
        print("  ! [E] lines in the log:")
        for l in errors[:10]:
            print(f"      {l}")
        return False
    return True


def variant_command(exe: Path, preset: dict, variant: str, width: int, height: int, out_png: Path) -> list:
    cmd = [str(exe), "--character", preset["character"], "--stage", preset["stage"], "--song", preset["song"],
           "--autoplay", "--seek", str(preset["seek"]), "--paused", "--no-physics",
           "--width", str(width), "--height", str(height),
           "--lighting", str(preset["lighting"]), "--dof", str(preset["dof"]),
           "--effect", "none", "--shader-pack", "none", "--upscaler", "none"]
    if preset.get("camera"):
        cmd += ["--camera", preset["camera"]]
    if variant == "offline_gi":
        cmd += ["--offline-still", str(out_png), "--offline-size", str(width), str(height)]
    else:
        render = {"classic": "raster", "raster": "raster", "rt": "rt", "pt": "pt"}[variant]
        # --capture-scene 1: the rendered scene only (no play bar / stats overlay). PT accumulates while paused,
        # so it gets more frames to converge.
        frames = "900" if render == "pt" else "90"
        cmd += ["--render", render, "--frames", frames, "--capture", str(out_png), "--capture-scene", "1"]
        if variant == "classic":
            cmd += ["--classic"]
    return cmd


def clip_command(exe: Path, preset: dict, variant: str, width: int, height: int, out_mp4: Path,
                 seconds: float) -> list:
    a = preset["seek"]
    b = preset["seek"] + seconds
    cmd = [str(exe), "--character", preset["character"], "--stage", preset["stage"], "--song", preset["song"],
           "--autoplay", "--no-physics",
           "--offline-video", str(out_mp4), "--offline-range", str(a), str(b),
           "--offline-renderer", CLIP_RENDERERS[variant],
           "--offline-size", str(width), str(height),
           "--offline-fps", "30", "--offline-quality", "2",
           "--width", str(width), "--height", str(height),
           "--lighting", str(preset["lighting"]), "--dof", str(preset["dof"]),
           "--effect", "none", "--shader-pack", "none", "--upscaler", "none"]
    if preset.get("camera"):
        cmd += ["--camera", preset["camera"]]
    if variant == "classic":
        cmd += ["--classic"]
    return cmd


# ---------------------------------------------------------------------------
# Composition
# ---------------------------------------------------------------------------

def _font(size: int):
    from PIL import ImageFont
    for name in ("Pretendard-Bold.otf", "Pretendard-SemiBold.otf"):
        p = REPO_ROOT / "assets" / "fonts" / name
        if p.is_file():
            try:
                return ImageFont.truetype(str(p), size)
            except Exception:
                pass
    return ImageFont.load_default()


def _draw_label(draw, xy, text, size, anchor="lt"):
    from PIL import Image
    font = _font(size)
    pad = max(6, size // 3)
    try:
        box = draw.textbbox((0, 0), text, font=font)
        tw, th = box[2] - box[0], box[3] - box[1]
    except Exception:
        tw, th = size * len(text) // 2, size
    x, y = xy
    draw.rectangle([x, y, x + tw + pad * 2, y + th + pad * 2], fill=(0, 0, 0))
    draw.text((x + pad, y + pad), text, fill=(255, 255, 255), font=font)


def load_panels(out_dir: Path, variants: list, size):
    from PIL import Image
    panels = {}
    for v in variants:
        p = out_dir / f"{v}.png"
        if not p.is_file():
            continue
        img = Image.open(p).convert("RGB")
        if size and img.size != size:
            img = img.resize(size, Image.Resampling.LANCZOS)
        panels[v] = img
    return panels


def compose_with_pillow(out_dir: Path, variants: list, labels: dict, width: int, height: int) -> bool:
    from PIL import Image, ImageDraw
    panels = load_panels(out_dir, variants, (width, height))
    if not panels:
        return False
    size = next(iter(panels.values())).size
    w, h = size
    label_size = max(16, h // 22)

    def save(img, stem):
        img.save(out_dir / f"{stem}.png")
        img.convert("RGB").save(out_dir / f"{stem}.jpg", format="JPEG", quality=90)

    # 2x2 grid of the first four variants
    grid = [v for v in GRID_VARIANTS if v in panels]
    if len(grid) == 4:
        sheet = Image.new("RGB", (w * 2, h * 2), (16, 16, 20))
        for i, v in enumerate(grid):
            x = (i % 2) * w
            y = (i // 2) * h
            sheet.paste(panels[v], (x, y))
            ImageDraw.Draw(sheet).rectangle([x, y, x + w - 1, y + h - 1], outline=(0, 0, 0))
            _draw_label(ImageDraw.Draw(sheet), (x + 8, y + 8), labels[v], label_size)
        save(sheet, "compare_2x2")

    # 1x4 strip
    if len(grid) == 4:
        strip = Image.new("RGB", (w * 4, h), (16, 16, 20))
        for i, v in enumerate(grid):
            strip.paste(panels[v], (i * w, 0))
            ImageDraw.Draw(strip).rectangle([i * w, 0, i * w + w - 1, h - 1], outline=(0, 0, 0))
            _draw_label(ImageDraw.Draw(strip), (i * w + 8, 8), labels[v], label_size)
        save(strip, "compare_strip")

    # hero split: left half classic, right half SPLIT_RIGHT (raster = what every GPU gets by default; real-time PT is
    # still grainy on alpha-blended materials)
    if "classic" in panels and SPLIT_RIGHT in panels:
        split = Image.new("RGB", (w, h), (16, 16, 20))
        split.paste(panels["classic"].crop((0, 0, w // 2, h)), (0, 0))
        split.paste(panels[SPLIT_RIGHT].crop((w // 2, 0, w, h)), (w // 2, 0))
        d = ImageDraw.Draw(split)
        d.rectangle([w // 2 - 1, 0, w // 2, h], fill=(255, 255, 255))
        _draw_label(d, (8, 8), labels["classic"], label_size)
        t = labels[SPLIT_RIGHT]
        box = d.textbbox((0, 0), t, font=_font(label_size))
        _draw_label(d, (w - (box[2] - box[0]) - 8 - max(6, label_size // 3), 8), t, label_size)
        save(split, "compare_split")
    return True


def compose_with_ffmpeg(out_dir: Path, variants: list, labels: dict) -> bool:
    ff = shutil.which("ffmpeg")
    if not ff:
        return False

    def inputs(vs):
        args = []
        for v in vs:
            args += ["-i", str(out_dir / f"{v}.png")]
        return args

    def label_filter(vs, line):
        parts = []
        for i, v in enumerate(vs):
            txt = labels[v].replace(":", "\\:").replace("'", "")
            parts.append(f"[{i}:v]drawtext=text='{txt}':x=10:y=10:fontsize=28:"
                         f"fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=8[v{i}]")
        ins = "".join(f"[v{i}]" for i in range(len(vs)))
        parts.append(f"{ins}{line}[out]")
        return ";".join(parts)

    ok = False
    grid = [v for v in GRID_VARIANTS if (out_dir / f"{v}.png").is_file()]
    try:
        if len(grid) == 4:
            fc = label_filter(grid, "xstack=inputs=4:layout=0_0|w0_0|0_h0|w0_h0")
            subprocess.run([ff, "-y", *inputs(grid), "-filter_complex", fc, "-map", "[out]",
                            str(out_dir / "compare_2x2.png")], check=True, capture_output=True)
            fc = label_filter(grid, "hstack=inputs=4")
            subprocess.run([ff, "-y", *inputs(grid), "-filter_complex", fc, "-map", "[out]",
                            str(out_dir / "compare_strip.png")], check=True, capture_output=True)
            ok = True
        if (out_dir / "classic.png").is_file() and (out_dir / f"{SPLIT_RIGHT}.png").is_file():
            # left half classic + right half SPLIT_RIGHT
            fc = ("[0:v]crop=iw/2:ih:0:0[l];[1:v]crop=iw/2:ih:iw/2:0[r];[l][r]hstack=inputs=2[out]")
            subprocess.run([ff, "-y", "-i", str(out_dir / "classic.png"), "-i", str(out_dir / f"{SPLIT_RIGHT}.png"),
                            "-filter_complex", fc, "-map", "[out]",
                            str(out_dir / "compare_split.png")], check=True, capture_output=True)
            ok = True
    except Exception as e:
        print(f"  ! ffmpeg composition failed: {e}")
        return False
    return ok


def jpeg_convert(out_dir: Path, stems: list):
    from PIL import Image
    for stem in stems:
        png = out_dir / f"{stem}.png"
        if png.is_file():
            Image.open(png).convert("RGB").save(out_dir / f"{stem}.jpg", format="JPEG", quality=90)


def compose(out_dir: Path, variants: list, labels: dict, width: int, height: int):
    try:
        import PIL  # noqa: F401
        have_pillow = True
    except Exception:
        have_pillow = False
    if have_pillow:
        if compose_with_pillow(out_dir, variants, labels, width, height):
            jpeg_convert(out_dir, [f"{v}" for v in variants])
            print("  composed with Pillow (PNG + JPEG q90)")
            return
    if compose_with_ffmpeg(out_dir, variants, labels):
        print("  composed with ffmpeg")
        return
    print("  ! no Pillow and no ffmpeg: kept the raw PNGs only (no composed comparison images)")


# ---------------------------------------------------------------------------

def main() -> int:
    global SPLIT_RIGHT
    ap = argparse.ArgumentParser(description="Regenerate the MMDX12 A/B comparison media")
    ap.add_argument("--exe", default="", help="MMDX12.exe (default: build, else build_dev)")
    ap.add_argument("--out", default=str(REPO_ROOT / "captures" / "media_kit"), help="output directory")
    ap.add_argument("--scene", default="default", help=f"scene preset: {', '.join(SCENE_PRESETS)}")
    ap.add_argument("--only", default="", help="comma-separated variants to capture: " + ",".join(VARIANTS))
    ap.add_argument("--size", nargs=2, type=int, metavar=("W", "H"), default=[1600, 900], help="capture size")
    ap.add_argument("--clips", action="store_true", help="also render ~8 s loops per real-time variant")
    ap.add_argument("--clip-seconds", type=float, default=8.0, help="loop length for --clips")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="per-run timeout in seconds (0 = per-variant default)")
    ap.add_argument("--dry-run", action="store_true", help="print the command lines only")
    ap.add_argument("--split-right", default=SPLIT_RIGHT, choices=VARIANTS[1:],
                    help="variant for the right half of compare_split (default: raster)")
    args = ap.parse_args()
    SPLIT_RIGHT = args.split_right

    if args.scene not in SCENE_PRESETS:
        print(f"unknown --scene '{args.scene}'; known: {', '.join(SCENE_PRESETS)}", file=sys.stderr)
        return 1
    preset = SCENE_PRESETS[args.scene]
    wanted = [v.strip() for v in args.only.split(",") if v.strip()] if args.only else list(VARIANTS)
    for v in wanted:
        if v not in VARIANTS:
            print(f"unknown --only variant '{v}'; known: {', '.join(VARIANTS)}", file=sys.stderr)
            return 1

    exe = find_exe(args.exe)
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    width, height = int(args.size[0]) & ~1, int(args.size[1]) & ~1
    bin_dir = exe.parent
    log_path = bin_dir / "mmdx12.log"
    ini_path = bin_dir / "mmdx12.ini"

    realtime_timeout = args.timeout if args.timeout > 0 else 240.0
    gi_timeout = args.timeout if args.timeout > 0 else 900.0

    print(f"MMDX12 media kit: scene={args.scene} ({preset['character']} / {preset['stage']} / {preset['song']} "
          f"@ {preset['seek']}s)  {width}x{height}  out={out_dir}")
    print(f"exe={exe}")

    # --dry-run: print the commands, no ini backup, no app runs.
    if args.dry_run:
        for v in wanted:
            print(f"[{v}] {LABELS[v]}")
            print("  " + " ".join(variant_command(exe, preset, v, width, height, out_dir / f"{v}.png")))
        if args.clips:
            for v in wanted:
                if v in CLIP_RENDERERS:
                    print(f"[{v}] clip")
                    print("  " + " ".join(clip_command(exe, preset, v, width, height,
                                                       out_dir / f"{v}.mp4", args.clip_seconds)))
        return 0

    ini_backup = bin_dir / "mmdx12.ini.mediakit.bak"
    have_ini = ini_path.is_file()
    if have_ini:
        shutil.copy2(ini_path, ini_backup)

    results = {}
    failed = []
    try:
        for v in wanted:
            print(f"[{v}] {LABELS[v]}")
            cmd = variant_command(exe, preset, v, width, height, out_dir / f"{v}.png")
            timeout = gi_timeout if v == "offline_gi" else realtime_timeout
            results[v] = run_capture(cmd, log_path, timeout, False)
            if not results[v]:
                failed.append(v)
        if args.clips:
            for v in wanted:
                if v not in CLIP_RENDERERS:
                    continue
                print(f"[{v}] clip ({CLIP_RENDERERS[v]}, {args.clip_seconds:.0f}s)")
                cmd = clip_command(exe, preset, v, width, height, out_dir / f"{v}.mp4", args.clip_seconds)
                if not run_capture(cmd, log_path, realtime_timeout, False):
                    failed.append(f"{v}:clip")
    finally:
        if have_ini:
            shutil.copy2(ini_backup, ini_path)
            ini_backup.unlink(missing_ok=True)
        else:
            # the run may have created one: remove it so the batch leaves no trace
            if ini_path.is_file():
                ini_path.unlink()

    captured = [v for v in VARIANTS if (out_dir / f"{v}.png").is_file()]
    if captured:
        compose(out_dir, captured, LABELS, width, height)

    print("\n=== media kit summary ===")
    for v in VARIANTS:
        if (out_dir / f"{v}.png").is_file():
            state = "captured" if results.get(v) else ("failed" if v in results else "existing")
            print(f"  {v:12} {LABELS[v]:20} {state}")
    if failed:
        print(f"  failed: {', '.join(failed)} (kept the other variants)")
    print(f"  output: {out_dir}")
    return 0 if not failed else 2


if __name__ == "__main__":
    sys.exit(main())
