#!/usr/bin/env python3
"""Capture before/after preview pairs for every public shader and effect pack.

Drives MMDX12 over MCP (named pipe), captures 1280x720 scene-only screenshots with
and without each pack, verifies pixel alignment and stability (< 1/255 mean abs diff),
saves JPEG quality 92 previews (preview_before.jpg + preview.jpg) into the pack folders, optionally bumps
the pack.json patch version (--bump), and builds a contact sheet in captures/pack_previews.
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# Set UTF-8 encoding for stdout/stderr on Windows
if sys.stdout:
    sys.stdout.reconfigure(encoding="utf-8")
if sys.stderr:
    sys.stderr.reconfigure(encoding="utf-8")

# Add tools directory to module search path for mcp_pipe
TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
WEB_REPO_ROOT = REPO_ROOT.parent / "MMDX12_Web"   # the website repo next to this one (--web-root)
APP_BIN_DIR = REPO_ROOT / "build_dev" / "bin"
if not (APP_BIN_DIR / "MMDX12.exe").is_file():
    APP_BIN_DIR = REPO_ROOT / "build" / "bin"
APP_EXE = APP_BIN_DIR / "MMDX12.exe"

sys.path.insert(0, str(TOOLS_DIR))
from mcp_pipe import MmdxPipe, McpError, list_pipes

# One shot per public pack. character / stage / song: load_scene substrings; seek: seconds; lighting_preset: 0..3
# (2 = concert: stage lights bright enough for the lens / glow packs); params: effect parameters for the AFTER shot
# (empty = pack defaults). texture_folder: per-character pack texture folder, relative to the repo.
def _shot(kind, character, stage, seek, lighting=0, **extra):
    return {"type": kind, "character": character, "stage": stage, "song": "catch_the_wave", "seek": seek,
            "lighting_preset": lighting, **extra}


SHOT_CONFIGS = {
    # surface packs: upper-body shots on the bright terrace
    "hoyo_toon": _shot("surface", "hutao", "terrace", 8.0),
    "hoyo_toon_v2": _shot("surface", "hutao", "terrace", 8.0, texture_folder=r"library\gamerip\hutao"),
    "starrail_face": _shot("surface", "Firefly", "terrace", 8.0,
                           texture_folder=r"library\gamerip\StarRailTextures\_sets\Firefly"),
    "nimble_toon": _shot("surface", "Felix", "terrace", 8.0),
    "project_sekai_toon": _shot("surface", "01_9001", "terrace", 8.0),   # Sekai Model Pack v4 Ichika (face SDF via MME refs)
    # effect packs: Sour Miku on the theater stage
    "auto_luminous": _shot("effect", "sour_miku", "theater", 8.0, 2,
                           params={"threshold": 0.35, "strength": 2.0}),   # the default threshold 1.0 barely triggers in MMD scenes
    "bokeh_dof": _shot("effect", "sour_miku", "theater", 8.0),
    "chromatic_aberration": _shot("effect", "sour_miku", "theater", 8.0),
    "crt_scanlines": _shot("effect", "sour_miku", "theater", 14.0),
    "film_grain": _shot("effect", "sour_miku", "theater", 8.0),
    "lens_dirt": _shot("effect", "sour_miku", "theater", 8.0),
    "lens_flare": _shot("effect", "sour_miku", "theater", 8.0, 2),
    "lens_reflection": _shot("effect", "sour_miku", "theater", 8.0),
    "screen_ao": _shot("effect", "sour_miku", "theater", 22.0),
    "eye_adaptation": _shot("effect", "sour_miku", "theater", 8.0, 2),
    # world packs: Rom, a full-body free camera (set_camera) so the ground layers show; BEFORE = no world effect
    "snow": _shot("world", "Rom_MMD", "pjsk_fireflower", 8.0, camera=([0, 10, 0], 25, 12, 45)),
    "rain": _shot("world", "Rom_MMD", "pjsk_fireflower", 8.0, camera=([0, 10, 0], 25, 8, 45)),
    "ground_mist": _shot("world", "Rom_MMD", "pjsk_fireflower", 8.0, camera=([0, 10, 0], 25, 12, 45)),
    "fireflies": _shot("world", "Rom_MMD", "pjsk_fireflower", 8.0, 3, camera=([0, 11, 0], 20, 6, 40)),
    "fireworks": _shot("world", "Rom_MMD", "pjsk_fireflower", 12.0, camera=([0, 30, 0], 15, -14, 70)),
    "aurora": _shot("world", "Rom_MMD", "pjsk_fireflower", 8.0, camera=([0, 30, 0], 15, -18, 60)),
    "sakura": _shot("world", "Rom_MMD", "terrace", 8.0, camera=([0, 10, 0], 25, 14, 45)),
    "confetti": _shot("world", "Rom_MMD", "terrace", 8.0, camera=([0, 11, 0], 20, 6, 42)),
}


def compute_mean_abs_diff(img1: Image.Image, img2: Image.Image) -> float:
    """Computes mean absolute difference per channel across all pixels (0..255 scale)."""
    im1 = img1.convert("RGB")
    im2 = img2.convert("RGB")
    if im1.size != im2.size:
        im2 = im2.resize(im1.size, Image.Resampling.LANCZOS)
    b1 = im1.tobytes()
    b2 = im2.tobytes()
    if len(b1) != len(b2):
        return 255.0
    total_diff = sum(abs(a - b) for a, b in zip(b1, b2))
    return total_diff / len(b1)


def resize_to_720p_if_needed(img: Image.Image) -> Image.Image:
    """Ensures image is 1280x720 using Lanczos downscale without cropping aspect."""
    target_w, target_h = 1280, 720
    if img.size == (target_w, target_h):
        return img
    return img.resize((target_w, target_h), Image.Resampling.LANCZOS)


def save_jpeg_92(img: Image.Image, out_path: Path):
    """Saves image as JPEG quality 92, sRGB."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    rgb = img.convert("RGB")
    rgb.save(out_path, format="JPEG", quality=92)


def bump_patch_version(current_ver: str) -> str:
    """Bumps semantic version patch number: 1.2.3 -> 1.2.4."""
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)(.*)$", current_ver.strip())
    if not m:
        raise ValueError(f"Invalid semver: {current_ver}")
    major, minor, patch, suffix = m.groups()
    new_patch = int(patch) + 1
    return f"{major}.{minor}.{new_patch}{suffix}"


def update_pack_version(pack_json_path: Path) -> tuple[str, str]:
    """Bumps version patch number in pack.json, preserving formatting."""
    content = pack_json_path.read_text(encoding="utf-8")
    data = json.loads(content)
    old_ver = data["version"]
    new_ver = bump_patch_version(old_ver)
    
    # Replace version string while preserving original indentation and formatting
    new_content = re.sub(
        r'("version"\s*:\s*")' + re.escape(old_ver) + r'(")',
        r"\g<1>" + new_ver + r"\2",
        content,
        count=1
    )
    pack_json_path.write_text(new_content, encoding="utf-8", newline="\n")
    return old_ver, new_ver


class AppController:
    """Manages connection to MMDX12 over MCP pipe."""
    def __init__(self, auto_launch: bool = True):
        self.app = None
        self.proc = None
        self.launched_by_us = False

        pipes = list_pipes()
        if pipes:
            print(f"Connecting to existing MMDX12 instance ({pipes[0]})...")
            self.app = MmdxPipe(name=pipes[0], wait=10.0)
        elif auto_launch:
            if not APP_EXE.is_file():
                raise RuntimeError(f"MMDX12 binary not found at {APP_EXE}")
            print(f"Launching MMDX12: {APP_EXE} --mcp --width 1280 --height 720...")
            self.proc = subprocess.Popen(
                [str(APP_EXE), "--mcp", "--width", "1280", "--height", "720"],
                cwd=str(APP_BIN_DIR)
            )
            self.launched_by_us = True
            print("Connecting to MCP pipe...")
            self.app = MmdxPipe(wait=30.0)
        else:
            raise RuntimeError("No MMDX12 MCP pipe found and auto_launch is disabled.")

    def call(self, tool: str, **args):
        return self.app.call(tool, **args)

    def quit(self):
        if self.app:
            try:
                print("Sending quit_app...")
                self.app.call("quit_app")
            except Exception as e:
                print(f"quit_app note: {e}")
            try:
                self.app.close()
            except Exception:
                pass
            self.app = None

        if self.proc:
            try:
                self.proc.wait(timeout=10)
                print(f"App process exited with code {self.proc.returncode}")
            except subprocess.TimeoutExpired:
                print("Terminating app process...")
                self.proc.terminate()
            self.proc = None


def capture_shot(controller: AppController, pack_id: str, cfg: dict,
                 seek_override: float = None, char_override: str = None,
                 stage_override: str = None, song_override: str = None,
                 lighting_override: int = None, temp_dir: Path = None):
    """Executes the capture steps for a single pack shot."""
    p_type = cfg["type"]
    character = char_override if char_override else cfg["character"]
    stage = stage_override if stage_override else cfg["stage"]
    song = song_override if song_override else cfg["song"]
    seek_time = seek_override if seek_override is not None else cfg["seek"]
    lighting_preset = lighting_override if lighting_override is not None else cfg.get("lighting_preset")
    tex_folder = str(REPO_ROOT / cfg["texture_folder"]) if cfg.get("texture_folder") else ""

    if temp_dir is None:
        temp_dir = Path(tempfile.gettempdir()) / "mmdx_previews"
    temp_dir.mkdir(parents=True, exist_ok=True)

    b1_path = temp_dir / f"{pack_id}_before1.png"
    af_path = temp_dir / f"{pack_id}_after.png"
    b2_path = temp_dir / f"{pack_id}_before2.png"

    # 1. load_scene
    print(f"[{pack_id}] Loading scene: {character} / {stage} / {song} (lighting={lighting_preset})...")
    controller.call("load_scene", character=character, stage=stage, song=song)

    # 2. pause, seek, wait_frames 90
    controller.call("pause")
    controller.call("seek", seconds=float(seek_time))
    if cfg.get("camera"):   # a free orbit camera instead of the song's (target, yaw, pitch, distance)
        target, yaw, pitch, dist = cfg["camera"]
        controller.call("set_camera", target=target, yaw_deg=yaw, pitch_deg=pitch, distance=dist)
    controller.call("wait_frames", n=90)

    # Helper to set BEFORE state
    def set_before():
        kwargs = {"effects": "none", "shader_pack": "default"}
        if p_type == "world":
            kwargs["world_stack"] = []
        if p_type == "surface":
            kwargs["shader_texture_folder"] = ""
        if lighting_preset is not None:
            kwargs["lighting_preset"] = int(lighting_preset)
        controller.call("set_render_settings", **kwargs)
        controller.call("wait_frames", n=90)

    # Helper to set AFTER state
    def set_after():
        if p_type == "surface":
            kwargs = {"shader_pack": pack_id, "shader_texture_folder": tex_folder, "effects": "none"}
        elif p_type == "world":
            entry = {"id": pack_id}
            if cfg.get("params"):
                entry["params"] = cfg["params"]
            kwargs = {"world_stack": [entry], "effects": "none", "shader_pack": "default"}
        else:
            entry = {"id": pack_id}
            if cfg.get("params"):
                entry["params"] = cfg["params"]
            kwargs = {"effect_stack": [entry], "shader_pack": "default"}
        if lighting_preset is not None:
            kwargs["lighting_preset"] = int(lighting_preset)
        controller.call("set_render_settings", **kwargs)
        controller.call("wait_frames", n=90)

    # 3. Capture BEFORE (1)
    set_before()
    controller.call("screenshot", scene_only=True, path=str(b1_path.resolve()))

    # 4. Capture AFTER
    set_after()
    controller.call("screenshot", scene_only=True, path=str(af_path.resolve()))

    # 5. Capture BEFORE (2)
    set_before()
    controller.call("screenshot", scene_only=True, path=str(b2_path.resolve()))

    img_b1 = Image.open(b1_path)
    img_af = Image.open(af_path)
    img_b2 = Image.open(b2_path)

    # Stability check (mean abs diff between before1 and before2)
    stab_diff = compute_mean_abs_diff(img_b1, img_b2)
    max_retries = 2
    retry_count = 0
    while stab_diff > 1.0 and retry_count < max_retries:
        retry_count += 1
        print(f"[{pack_id}] Notice: stability diff {stab_diff:.4f} > 1.0, retrying with wait_frames 180 (retry {retry_count})...")
        controller.call("wait_frames", n=180)
        set_before()
        controller.call("screenshot", scene_only=True, path=str(b1_path.resolve()))
        set_after()
        controller.call("screenshot", scene_only=True, path=str(af_path.resolve()))
        set_before()
        controller.call("screenshot", scene_only=True, path=str(b2_path.resolve()))
        img_b1 = Image.open(b1_path)
        img_af = Image.open(af_path)
        img_b2 = Image.open(b2_path)
        stab_diff = compute_mean_abs_diff(img_b1, img_b2)

    # Pack diff (before vs after)
    pack_diff = compute_mean_abs_diff(img_b1, img_af)

    img_b1_720p = resize_to_720p_if_needed(img_b1)
    img_af_720p = resize_to_720p_if_needed(img_af)

    notes = []
    if retry_count > 0:
        notes.append(f"retried {retry_count}x")
    if p_type in ("effect", "world") and pack_diff < 1.0:
        notes.append("subtle effect")

    return {
        "pack_id": pack_id,
        "scene": f"{character}/{stage}/{song} @ {seek_time:.1f}s",
        "before_img": img_b1_720p,
        "after_img": img_af_720p,
        "diff_abs": pack_diff,
        "diff_norm": pack_diff / 255.0,
        "stab_abs": stab_diff,
        "stab_norm": stab_diff / 255.0,
        "notes": "; ".join(notes) if notes else "ok",
    }


def generate_contact_sheet(results: list[dict], out_path: Path):
    """Generates a contact sheet contact.jpg where each pack is one row: before | after, labelled."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    num_rows = len(results)
    if num_rows == 0:
        return

    # Scale down thumbnails to 640x360 for compact contact sheet
    thumb_w, thumb_h = 640, 360
    header_h = 40
    row_h = thumb_h + header_h + 10
    total_w = thumb_w * 2 + 30
    total_h = row_h * num_rows + 20

    sheet = Image.new("RGB", (total_w, total_h), color=(24, 24, 28))
    draw = ImageDraw.Draw(sheet)

    font_path = REPO_ROOT / "assets" / "fonts" / "Pretendard-Bold.otf"
    try:
        font_title = ImageFont.truetype(str(font_path), 20)
        font_sub = ImageFont.truetype(str(font_path), 14)
    except Exception:
        font_title = ImageFont.load_default()
        font_sub = ImageFont.load_default()

    for idx, res in enumerate(results):
        y_offset = 10 + idx * row_h
        pack_id = res["pack_id"]
        title_text = f"[{idx+1}/{num_rows}]  {pack_id}  ({res['scene']})"
        diff_text = f"diff: {res['diff_abs']:.2f} ({res['diff_norm']:.4f})  |  stability: {res['stab_abs']:.2f} ({res['stab_norm']:.4f})"

        draw.text((15, y_offset + 6), title_text, fill=(255, 255, 255), font=font_title)
        draw.text((thumb_w + 30, y_offset + 10), diff_text, fill=(180, 180, 190), font=font_sub)

        # Before thumbnail
        b_thumb = res["before_img"].resize((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        sheet.paste(b_thumb, (10, y_offset + header_h))
        # After thumbnail
        a_thumb = res["after_img"].resize((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        sheet.paste(a_thumb, (thumb_w + 20, y_offset + header_h))

        # Overlay labels on images
        draw.rectangle([15, y_offset + header_h + 10, 110, y_offset + header_h + 36], fill=(0, 0, 0, 180))
        draw.text((25, y_offset + header_h + 14), "BEFORE", fill=(255, 255, 255), font=font_sub)

        draw.rectangle([thumb_w + 25, y_offset + header_h + 10, thumb_w + 115, y_offset + header_h + 36], fill=(0, 0, 0, 180))
        draw.text((thumb_w + 38, y_offset + header_h + 14), "AFTER", fill=(100, 220, 255), font=font_sub)

    sheet.save(out_path, format="JPEG", quality=92)
    print(f"Contact sheet saved: {out_path}")


def main():
    parser = argparse.ArgumentParser(description="Capture shader/effect pack before/after previews")
    parser.add_argument("--only", type=str, default="", help="Comma-separated pack IDs to run")
    parser.add_argument("--out-dir", type=str, default="", help="Write preview pairs to this DIR instead of pack folders")
    parser.add_argument("--dry-run", action="store_true", help="Print shot table without capturing")
    parser.add_argument("--seek", type=float, default=None, help="Override seek time for the selected shots (scouting)")
    parser.add_argument("--character", type=str, default="", help="Override character for the selected shots")
    parser.add_argument("--stage", type=str, default="", help="Override stage for the selected shots")
    parser.add_argument("--song", type=str, default="", help="Override song for the selected shots")
    parser.add_argument("--lighting-preset", type=int, default=None, help="Override lighting preset (0..3)")
    parser.add_argument("--bump", action="store_true", help="Bump the pack.json patch version (installed apps cache previews by id + version)")
    parser.add_argument("--web-root", type=str, default="", help="Website repo (default: ../MMDX12_Web)")
    parser.add_argument("--no-quit", action="store_true", help="Do not quit MMDX12 at the end")
    parser.add_argument("--exe", type=str, default="", help="MMDX12.exe to launch (default: build_dev, else build)")
    args = parser.parse_args()

    global APP_EXE, APP_BIN_DIR
    if args.exe:
        APP_EXE = Path(args.exe).resolve()
        APP_BIN_DIR = APP_EXE.parent

    global WEB_REPO_ROOT
    if args.web_root:
        WEB_REPO_ROOT = Path(args.web_root)
    selected_ids = [x.strip() for x in args.only.split(",") if x.strip()] if args.only else list(SHOT_CONFIGS.keys())
    for pid in selected_ids:
        if pid not in SHOT_CONFIGS:
            print(f"Error: unknown pack ID '{pid}'. Known: {list(SHOT_CONFIGS.keys())}", file=sys.stderr)
            return 1

    print(f"Running preview capture for {len(selected_ids)} pack(s): {', '.join(selected_ids)}")
    if args.dry_run:
        print("[Dry run] Shot configuration:")
        for pid in selected_ids:
            cfg = SHOT_CONFIGS[pid]
            st = args.seek if args.seek is not None else cfg["seek"]
            ch = args.character if args.character else cfg["character"]
            sg = args.stage if args.stage else cfg["stage"]
            sn = args.song if args.song else cfg["song"]
            lp = args.lighting_preset if args.lighting_preset is not None else cfg.get("lighting_preset")
            print(f"  - {pid:22} {cfg['type']:8} {ch:12} {sg:12} {sn:16} seek={st}s lighting={lp}")
        return 0

    controller = AppController(auto_launch=True)
    results = []
    try:
        for pid in selected_ids:
            cfg = SHOT_CONFIGS[pid]
            res = capture_shot(
                controller, pid, cfg,
                seek_override=args.seek,
                char_override=args.character,
                stage_override=args.stage,
                song_override=args.song,
                lighting_override=args.lighting_preset
            )
            results.append(res)
            print(f"[{pid}] diff={res['diff_abs']:.3f} (norm {res['diff_norm']:.5f}), stab={res['stab_abs']:.3f} (norm {res['stab_norm']:.5f}) - {res['notes']}")

        # Verify recent logs for errors
        logs = controller.call("get_recent_logs", lines=100)
        error_lines = [l for l in logs.get("lines", []) if "[E]" in l]
        if error_lines:
            print("\nWARNING: Errors detected in recent logs:")
            for l in error_lines:
                print(f"  {l}")

        # Saving outputs
        captures_dir = REPO_ROOT / "captures" / "pack_previews"
        if args.out_dir:
            out_dir = Path(args.out_dir)
            out_dir.mkdir(parents=True, exist_ok=True)
            for res in results:
                pid = res["pack_id"]
                save_jpeg_92(res["before_img"], out_dir / f"{pid}_before.jpg")
                save_jpeg_92(res["after_img"], out_dir / f"{pid}_after.jpg")
            print(f"\nWrote {len(results)*2} images to {out_dir}")
        else:
            captures_dir.mkdir(parents=True, exist_ok=True)
            print("\nUpdating pack folders, versions, and captures...")
            for res in results:
                pid = res["pack_id"]
                b_img = res["before_img"]
                a_img = res["after_img"]

                # captures/pack_previews/<id>_before.jpg / <id>_after.jpg
                save_jpeg_92(b_img, captures_dir / f"{pid}_before.jpg")
                save_jpeg_92(a_img, captures_dir / f"{pid}_after.jpg")

                # website repo: shader-packs/<id>/
                web_pack_dir = WEB_REPO_ROOT / "shader-packs" / pid
                target_dirs = [web_pack_dir]

                # built-in packs also live in shaders/packs/<id>/
                if pid in ("hoyo_toon", "hoyo_toon_v2", "snow"):
                    app_pack_dir = REPO_ROOT / "shaders" / "packs" / pid
                    target_dirs.append(app_pack_dir)

                for tdir in target_dirs:
                    if not tdir.is_dir():
                        print(f"Warning: pack directory does not exist: {tdir}")
                        continue
                    save_jpeg_92(b_img, tdir / "preview_before.jpg")
                    save_jpeg_92(a_img, tdir / "preview.jpg")
                    old_png = tdir / "preview.png"
                    if old_png.is_file():
                        old_png.unlink()
                        print(f"[{pid}] Deleted old {old_png}")
                    
                    if args.bump:
                        pack_json = tdir / "pack.json"
                        if pack_json.is_file():
                            old_v, new_v = update_pack_version(pack_json)
                            print(f"[{pid}] Bumped version in {tdir.name}: {old_v} -> {new_v}")

            # Build full list of 13 packs for contact sheet
            all_contact_rows = []
            results_by_id = {r["pack_id"]: r for r in results}
            for pid in SHOT_CONFIGS.keys():
                if pid in results_by_id:
                    all_contact_rows.append(results_by_id[pid])
                else:
                    b_file = captures_dir / f"{pid}_before.jpg"
                    a_file = captures_dir / f"{pid}_after.jpg"
                    if b_file.is_file() and a_file.is_file():
                        im_b = Image.open(b_file)
                        im_a = Image.open(a_file)
                        d = compute_mean_abs_diff(im_b, im_a)
                        cfg = SHOT_CONFIGS[pid]
                        all_contact_rows.append({
                            "pack_id": pid,
                            "scene": f"{cfg['character']}/{cfg['stage']}/{cfg['song']} @ {cfg['seek']:.1f}s",
                            "before_img": im_b,
                            "after_img": im_a,
                            "diff_abs": d,
                            "diff_norm": d / 255.0,
                            "stab_abs": 0.0,
                            "stab_norm": 0.0,
                            "notes": "existing"
                        })

            contact_sheet_path = captures_dir / "contact.jpg"
            generate_contact_sheet(all_contact_rows, contact_sheet_path)

        # Print report summary table
        print("\n=== SUMMARY REPORT ===")
        print(f"{'Pack':<24} {'Scene (Char/Stage/Song @ Seek)':<42} {'Diff (Before vs After)':<24} {'Stability Diff':<18} {'Notes'}")
        print("-" * 125)
        for res in results:
            diff_str = f"{res['diff_abs']:.2f} ({res['diff_norm']:.4f})"
            stab_str = f"{res['stab_abs']:.2f} ({res['stab_norm']:.4f})"
            print(f"{res['pack_id']:<24} {res['scene']:<42} {diff_str:<24} {stab_str:<18} {res['notes']}")
        print("-" * 125)

    finally:
        if not args.no_quit:
            controller.quit()

    return 0


if __name__ == "__main__":
    sys.exit(main())
