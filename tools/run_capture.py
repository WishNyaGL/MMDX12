#!/usr/bin/env python3
"""The only supported way to launch MMDX12.exe from an agent / script.

Argument lists that cannot reach a scene (or cannot quit by themselves) are refused before a window
opens. A launchable run is started under a watchdog that tails `<exe dir>/mmdx12.log`:

  * kill when `STARTUP first frame` (and, for play-scene runs, `motion bound`) does not appear within
    `--first-frame-timeout` seconds (extended once by 60 s while a library rescan is running);
  * kill immediately on a `[W] unknown command line argument` line (the argument never reached the app);
  * hard kill at `--timeout` seconds.

The whole process tree is killed on every exit path, including Ctrl+C. Standard library only.

    python tools/run_capture.py [--exe build/bin/MMDX12.exe] [--first-frame-timeout 60] [--timeout 120]
        [--allow-gui] [--allow-library] -- <app args>
    python tools/run_capture.py --selftest

`--allow-gui` starts a deliberate interactive run (no watchdog deadline, nothing to reach); wrapper options stop
at the first argument that is not one of them, and `--` is optional.

Exit codes: 0 = the app quit by itself, reached its target and its --capture file exists (and is new);
1 = the run failed (killed, crashed, target missing, capture missing/stale); 2 = refused before launching.
"""
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join("build", "bin", "MMDX12.exe")
EXIT_OK, EXIT_FAILED, EXIT_REFUSED = 0, 1, 2
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)

# App.cpp ParseCommandLine: options that consume one value ...
VALUE_FLAGS = {
    "--library", "--character", "--stage", "--song", "--formation", "--song-audio", "--stage-variant",
    "--seek", "--benchmark", "--bench-frames", "--bench-spp", "--frames", "--capture", "--capture-scene",
    "--ui-script", "--width", "--height", "--screen", "--gallery-index", "--ui-scale", "--project", "--theme", "--lang",
    "--lighting", "--quality", "--render", "--shading", "--upscaler", "--upscale-quality", "--dlss-model",
    "--dlss-rr", "--frame-gen", "--dof", "--volumetric", "--volumetric-density", "--bloom-conv",
    "--bloom-kernel", "--bloom-blend", "--bloom-depth-mask", "--bloom-intensity", "--bloom-threshold",
    "--bloom-radius", "--auto-luminous", "--lut", "--motion-lighting", "--pack-index", "--shader-pack",
    "--material-source", "--pbr-debug", "--effect", "--world", "--look", "--mme", "--mme-model",
    "--mme-object", "--mme-panel", "--mme-auto", "--mme-global", "--ray-global", "--mme-workflow",
    "--stage-mme", "--offline-still", "--offline-video", "--offline-spp", "--studio-renderer",
    "--offline-prepass", "--offline-fps", "--offline-bitrate", "--offline-quality", "--offline-renderer",
    "--offline-alpha", "--camera", "--update-feed", "--update-relaunch-frames", "--apply-wait", "--component",
    "--mme-model-all",
}
# ... and the two that consume two.
TWO_VALUE_FLAGS = {"--offline-range", "--offline-size"}
# Offline / benchmark modes that quit the app by themselves (no --frames needed).
SELF_QUIT_FLAGS = {"--offline-still", "--offline-video", "--offline-probe"}
SELF_QUIT_BENCHMARKS = {"dx12-gi-render"}   # the real-time categories wait on their result screen
# App::ApplyStartScreen / App.h: the --screen values the app acts on (anything else falls back to the lobby).
SCREENS = {"select", "bench", "bench-gi", "stages", "songs", "shaders", "shaders-online", "shaders-mme", "mme",
           "settings", "studio", "video", "gallery"}
# src/app/Benchmark.h kBenchCategories.
BENCHMARKS = {"dx12-raster-fhd", "dx12-raster-4k", "dx12-rt-fhd", "dx12-rt-4k", "dx12-pt-fhd", "dx12-pt-4k",
              "dx12-gi-render"}

LOG_NAME = "mmdx12.log"
FIRST_FRAME_LINE = "STARTUP first frame"
MOTION_BOUND_LINE = "motion bound:"           # SceneLoader; "stage motion bound:" is another marker
UNKNOWN_ARG_LINE = "unknown command line argument"
LIBRARY_CACHE = os.path.join("cache", "library_result.json")
POLL_SECONDS = 0.2


# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------

def parse_app_args(args):
    """(values, present): the app's own command line, read like App.cpp does (last value wins)."""
    values, present = {}, set()
    i = 0
    while i < len(args):
        tok = args[i]
        if not tok.startswith("--"):
            i += 1
            continue
        present.add(tok)
        if tok in TWO_VALUE_FLAGS:
            values[tok] = args[i + 1:i + 3]
            i += 3
        elif tok in VALUE_FLAGS:
            values[tok] = args[i + 1] if i + 1 < len(args) else ""
            i += 2
        else:
            i += 1
    return values, present


def to_int(value, default=0):
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def has_flag(app_args, flag):
    """True when `flag` appears as an argument (not merely as another option's value)."""
    for tok in app_args:
        if tok == flag:
            return True
    return False


def apply_defaults(app_args):
    """--width/--height default to 1600 x 900 (pinned A/B window, independent of mmdx12.ini)."""
    out = list(app_args)
    if not has_flag(out, "--width"):
        out += ["--width", "1600"]
    if not has_flag(out, "--height"):
        out += ["--height", "900"]
    return out


def load_library(exe_dir):
    """<exe dir>/cache/library_result.json -> (dict, None) or (None, error text)."""
    path = os.path.join(exe_dir, LIBRARY_CACHE)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        return None, f"cannot read the library cache {path} ({exc})"
    if not isinstance(data, dict):
        return None, f"the library cache {path} is not an object"
    for key in ("characters", "stages", "songs"):
        if not isinstance(data.get(key), list):
            return None, f"the library cache {path} has no '{key}' list"
    return data, None


def find_entry(entries, needle):
    """The app's match: case-insensitive substring of id or displayName; the FIRST match wins."""
    want = needle.lower()
    if not want:
        return None
    for entry in entries:
        if want in str(entry.get("id", "")).lower() or want in str(entry.get("displayName", "")).lower():
            return entry
    return None


def sample_ids(entries, needle, limit=3):
    """A few ids that share a token with `needle`, for a useful refusal message."""
    tokens = [t for t in re.split(r"[\\/\s_.-]+", needle.lower()) if len(t) >= 3]
    out = []
    for entry in entries:
        ident = str(entry.get("id", ""))
        low = ident.lower()
        if any(t in low for t in tokens) or not tokens:
            out.append(ident)
        if len(out) >= limit:
            break
    return out


def part_count(song):
    parts = song.get("parts")
    return len(parts) if isinstance(parts, list) else 1


# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------

def classify(app_args):
    """What kind of run the argument list asks for."""
    values, present = parse_app_args(app_args)
    autoplay = "--autoplay" in present
    character = values.get("--character", "")
    song = values.get("--song", "")
    screen = values.get("--screen", "")
    project = values.get("--project", "")
    if not screen and project:
        screen = "studio"           # App.cpp: --project sets --screen studio
    frames = to_int(values.get("--frames"))
    benchmark = values.get("--benchmark", "")
    offline_flags = present & SELF_QUIT_FLAGS
    return {
        "values": values,
        "present": present,
        "autoplay": autoplay,
        "character": character,
        "song": song,
        "stage": values.get("--stage", ""),
        "screen": screen,
        "frames": frames,
        "benchmark": benchmark,
        "offline": bool(offline_flags),
        "offline_flags": sorted(offline_flags),
        "project": project,
        # a real play-scene load (the studio / project screens load their own models instead)
        "scene": bool(autoplay and character and song and screen != "studio" and not project),
        "capture": values.get("--capture", ""),
    }


def check_preflight(app_args, library, allow_library=False, allow_gui=False):
    """Every reason the argument list must not be launched (empty list = launchable)."""
    info = classify(app_args)
    present, values = info["present"], info["values"]
    issues = []

    if "--library" in present and not allow_library:
        issues.append("--library is refused: agents must use the real library (pass --allow-library only for "
                      "a deliberate isolated library)")

    if info["autoplay"] and not (info["character"] and info["song"]):
        issues.append("--autoplay needs both --character and --song: without them the app stays in the lobby "
                      "and --frames never counts")

    # The cache validates the names given; a run that names nothing (a --screen capture) needs no cache: the app
    # itself rescans the library on a build dir's first start and writes it.
    needs_names = bool(info["character"] or info["song"] or
                       (info["stage"] and info["stage"].strip().lower() != "none"))
    if library is None and needs_names:
        issues.append("no library cache: run the app once in this build dir so it writes "
                      + LIBRARY_CACHE + " (the cache validates --character/--song/--stage names)")
        return issues
    if library is None:
        return issues   # a screen-only capture: nothing to validate against the cache

    if info["character"]:
        if find_entry(library["characters"], info["character"]) is None:
            issues.append(_nomatch("--character", info["character"], library["characters"]))
    if info["stage"] and info["stage"].strip().lower() != "none":
        if find_entry(library["stages"], info["stage"]) is None:
            issues.append(_nomatch("--stage", info["stage"], library["stages"]) +
                          " (--stage none = no stage; MMDX stages appear as 'stages:<folder>' once scanned)")
    if info["song"]:
        song = find_entry(library["songs"], info["song"])
        if song is None:
            issues.append(_nomatch("--song", info["song"], library["songs"]))
        elif part_count(song) >= 2 and not values.get("--formation", "").strip() and info["screen"] != "studio":
            # the lobby / video-dialog load opens the formation modal for a unit song; the studio loads its own
            # models and ignores the parts (App::StartLobbyLoad vs App::StartStudioLoad)
            issues.append(f"--song '{song.get('id')}' has {part_count(song)} dance parts: pass --formation "
                          "(one entry per part, a character name substring or '-' for empty; '*' marks the center)")

    if info["offline"]:
        # The render job only starts from a loaded scene: a play scene (--autoplay), an open studio project,
        # or the lobby's video dialog with --character/--song (App.cpp starts it directly then).
        if info["scene"] or info["project"] or info["screen"] == "studio":
            pass
        elif info["screen"] == "video" and info["character"] and info["song"]:
            pass
        else:
            issues.append("an offline render (--offline-still/--offline-video/--offline-probe) needs a scene to render: "
                          "--autoplay --character .. --song .., --screen studio (or --project p.mmdxproj), or "
                          "--screen video --character .. --song ..")

    if info["screen"] and info["screen"] not in SCREENS:
        issues.append(f"--screen '{info['screen']}' is not a screen the app knows (" + "|".join(sorted(SCREENS)) +
                      "): it would fall back to the lobby and never quit")
    if info["benchmark"] and info["benchmark"].lower() not in BENCHMARKS:
        issues.append(f"--benchmark '{info['benchmark']}' is not a category (App.cpp falls back to the first one): " +
                      "|".join(sorted(BENCHMARKS)))

    if not allow_gui:
        issues.extend(_quit_issues(info))
    frames_issue = _frames_issue(info, allow_gui)
    if frames_issue:
        issues.append(frames_issue)              # never a scene: --frames can never count, even interactively
    return issues


def _nomatch(flag, needle, entries):
    text = f"{flag} '{needle}' matches nothing in the library (case-insensitive substring of id or display name)"
    samples = sample_ids(entries, needle)
    if samples:
        text += "; similar ids: " + ", ".join(samples)
    return text


def _frames_issue(info, allow_gui=False):
    """--frames only counts inside a scene or with --screen: a run without one never quits (always refused)."""
    if "--frames" not in info["present"]:
        return None
    if info["frames"] <= 0:
        if allow_gui:
            return None                              # an interactive run does not need counted frames
        return "--frames N needs a positive N: 0 never reaches the quit condition"
    if info["scene"] or info["screen"] or info["benchmark"]:
        return None
    if info["offline"]:
        return None                                  # the render job quits by itself; --frames is harmless
    return ("--frames N counts only inside a scene or with --screen: this run would never quit "
            "(add --autoplay --character .. --song .., or --screen <select|studio|bench|video>)")


def _quit_issues(info):
    """Refusals for runs that would leave a window up until the hard timeout."""
    screen = info["screen"]
    bench = info["benchmark"]
    if info["offline"]:
        return []                                    # the render job quits the app when it finishes
    if bench.lower() in SELF_QUIT_BENCHMARKS:
        return []                                    # the GI render benchmark quits by itself
    if info["frames"] > 0:
        return []                                    # counted frames make it quit (see _frames_issue)
    if info["scene"]:
        return ["a play-scene run needs --frames N to quit by itself (or an offline mode: --offline-still / "
                "--offline-video); pass --allow-gui for a deliberate interactive run"]
    if screen:
        return [f"--screen {screen} needs --frames N to quit by itself (it counts there); pass --allow-gui for a "
                "deliberate interactive run"]
    if bench:
        return [f"--benchmark {bench} shows its result screen and waits: add --frames N (real-time categories, "
                "e.g. --frames 100000) or use --benchmark dx12-gi-render (quits by itself); pass --allow-gui for an "
                "interactive run"]
    if info["character"] or info["song"] or info["stage"]:
        return ["no scene: --character/--song only preselect lobby cards; add --autoplay (with --character and "
                "--song) and --frames N, or --screen <select|studio|bench|video> --frames N"]
    return ["no way to quit by itself: give a scene (--autoplay --character .. --song .. --frames N), "
            "--screen <select|studio|bench|video> --frames N, an offline render (--offline-still/--offline-video) "
            "or --benchmark dx12-gi-render; pass --allow-gui for a deliberate interactive run"]


def required_markers(info):
    """(start, done): lines this run must log to count as started / finished."""
    start = [FIRST_FRAME_LINE]
    # `motion bound` is SceneLoader's play-scene binding; the studio logs its own load messages instead.
    if info["scene"] and info["screen"] != "studio":
        start.append(MOTION_BOUND_LINE)
    if "--ui-script" in info["present"]:
        start.append("ui script:")       # the script was read (a bad path logs an [E] line and nothing else)
    done = []
    if info["offline"]:
        done.append("offline render finished")
    if info["benchmark"]:
        done.append("BENCHMARK ")
    return start, done


# ---------------------------------------------------------------------------
# Process / log helpers
# ---------------------------------------------------------------------------

def other_app_running(exe, retries=2, wait=1.0):
    """True when another MMDX12.exe (any build dir) is running: one window at a time.

    Checked twice a second apart: a run that just finished is still exiting for a moment.
    """
    names = {os.path.basename(exe).lower(), "mmdx12.exe"}
    for attempt in range(retries):
        for name in sorted(names):
            try:
                out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"], capture_output=True,
                                     text=True, timeout=30, creationflags=CREATE_NO_WINDOW).stdout or ""
            except Exception:
                return False                          # tasklist unavailable: do not block the run
            if name in out.lower():
                break
        else:
            return False
        if attempt + 1 < retries:
            time.sleep(wait)
    return True


def kill_tree(proc):
    """Kill the app and its children (the app may spawn an update applier)."""
    if proc is None:
        return
    try:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, timeout=30,
                       creationflags=CREATE_NO_WINDOW)
    except Exception:
        pass
    try:
        proc.kill()
    except Exception:
        pass


class LogTail:
    """Incremental reader for <exe dir>/mmdx12.log; waits for the file this run creates."""

    BANNER = re.compile(r"^\[I\] MMDX12 ")

    def __init__(self, path, skip_existing=False):
        self.path = path
        self.offset = 0
        self.pending = b""
        if skip_existing:                    # the previous log could not be archived: ignore what is in it
            try:
                self.offset = os.stat(self.path).st_size
            except OSError:
                self.offset = 0

    def poll(self):
        """New complete lines written by this run (empty until the app creates the log)."""
        try:
            size = os.stat(self.path).st_size
        except OSError:
            return []                                # not created yet
        if size < self.offset:
            self.offset = 0                          # truncated / recreated
        if size == self.offset:
            return []
        try:
            with open(self.path, "rb") as f:
                f.seek(self.offset)
                data = f.read(size - self.offset)
        except OSError:
            return []
        self.offset = size
        data = self.pending + data
        self.pending = b""
        if not data.endswith(b"\n"):
            cut = data.rfind(b"\n")
            if cut < 0:
                self.pending = data
                return []
            self.pending, data = data[cut + 1:], data[:cut + 1]
        lines = [line.decode("utf-8", "replace").rstrip("\r") for line in data.split(b"\n") if line]
        # an appended log (an updater relaunch keeps the file) would repeat the previous run's markers
        for i in range(len(lines) - 1, 0, -1):
            if self.BANNER.match(lines[i]):
                lines = lines[i:]
                break
        return lines


def archive_log(log_path):
    """Move the previous run's log to mmdx12.previous.log (the updater's convention) so the new one is fresh."""
    try:
        if os.path.isfile(log_path):
            os.replace(log_path, log_path.replace("mmdx12.log", "mmdx12.previous.log"))
            return True
    except OSError:
        pass
    return False


def rescan_in_progress(text):
    return "library scan: storage" in text and not re.search(r"library scan: \d+ characters", text)


def format_size(path):
    try:
        size = os.path.getsize(path)
    except OSError:
        return ""
    return f"{size / 1048576:.1f} MB" if size >= 1048576 else f"{size / 1024:.0f} KB"


def file_stamp(path):
    """(size, mtime_ns) for a stale-file check, or None when the file does not exist."""
    try:
        st = os.stat(path)
        return st.st_size, st.st_mtime_ns
    except OSError:
        return None


# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

def run_app(exe, app_args, log_path, first_frame_timeout, timeout, info, interactive=False):
    started = time.monotonic()
    archived = archive_log(log_path)      # the new run writes a fresh mmdx12.log
    tail = LogTail(log_path, skip_existing=not archived)
    capture_before = file_stamp(info["capture"]) if info["capture"] else None
    start_markers, done_markers = required_markers(info)
    if interactive:                      # a deliberate interactive run: no target, no deadline (the owner closes it)
        start_markers, done_markers = [], []
    lines = []
    text = ""
    killed = None
    first_frame_deadline = None if interactive else started + first_frame_timeout
    hard_deadline = None if interactive or timeout <= 0 else started + timeout
    extended = False

    try:
        proc = subprocess.Popen([exe] + list(app_args))
    except OSError as exc:
        return {"killed": f"cannot start {exe}: {exc}", "rc": None, "elapsed": 0.0, "lines": [], "text": "",
                "start": start_markers, "done": done_markers,
                "missing": list(start_markers) + list(done_markers), "capture_before": capture_before}
    def take(new_lines):
        nonlocal text
        if not new_lines:
            return
        lines.extend(new_lines)
        text += "\n".join(new_lines) + "\n"

    try:
        while True:
            rc = proc.poll()
            if rc is not None:
                break
            take(tail.poll())
            unknown = next((l for l in lines if UNKNOWN_ARG_LINE in l), None)
            if unknown:
                killed = f"unknown command line argument: {unknown.split(UNKNOWN_ARG_LINE, 1)[-1].strip(' :')}"
                break
            now = time.monotonic()
            missing_start = [m for m in start_markers if m not in text]
            if missing_start and first_frame_deadline is not None and now >= first_frame_deadline:
                if not extended and rescan_in_progress(text):
                    extended = True
                    first_frame_deadline = now + 60.0
                    print("run_capture: library rescan in progress; first-frame deadline extended by 60 s")
                else:
                    killed = (f"no '{missing_start[0]}' within {first_frame_timeout:g} s"
                              + (" (library rescan extended once)" if extended else "")
                              + f"; missing: {', '.join(missing_start)}")
                    break
            if hard_deadline is not None and now >= hard_deadline:
                killed = f"hard timeout {timeout:g} s"
                break
            time.sleep(POLL_SECONDS)
        take(tail.poll())
        rc = proc.poll()
    except KeyboardInterrupt:
        killed = "Ctrl+C"
        rc = None
    finally:
        if proc.poll() is None:
            kill_tree(proc)
        try:
            proc.wait(timeout=15)
        except Exception:
            pass
    take(tail.poll())
    if killed is None:                   # a fast exit may log the unknown-argument line after the loop's last read
        unknown = next((l for l in lines if UNKNOWN_ARG_LINE in l), None)
        if unknown:
            killed = f"unknown command line argument: {unknown.split(UNKNOWN_ARG_LINE, 1)[-1].strip(' :')}"
    missing = [m for m in start_markers + done_markers if m not in text]
    return {"killed": killed, "rc": proc.returncode, "elapsed": time.monotonic() - started, "lines": lines,
            "start": start_markers, "done": done_markers, "missing": missing, "text": text,
            "capture_before": capture_before}


def report(result, info):
    lines = result["lines"]
    problems = []
    if result["killed"]:
        problems.append(f"killed: {result['killed']}")
    elif result["rc"] != 0:
        problems.append(f"the app exited with code {result['rc']} on its own")
    if result["missing"]:
        problems.append("target not reached: " + ", ".join(result["missing"]))

    capture, capture_ok, capture_note = "", False, ""
    if info["capture"]:
        capture = os.path.abspath(info["capture"])
        capture_ok = os.path.isfile(capture)
        if not capture_ok:
            problems.append(f"capture missing: {capture}")
            capture_note = "MISSING"
        elif result.get("capture_before") == file_stamp(capture):
            capture_ok = False
            capture_note = "STALE: not rewritten by this run"
            problems.append(f"capture was not rewritten by this run (stale): {capture}")
        else:
            capture_note = format_size(capture)
    reached = result["text"]
    if info["scene"]:
        scene_text = "yes" if MOTION_BOUND_LINE in reached and FIRST_FRAME_LINE in reached else "no"
    elif FIRST_FRAME_LINE in reached:
        scene_text = "yes (first frame)"     # a screen / studio / offline run: nothing to bind
    else:
        scene_text = "no"

    print()
    if result["killed"]:
        exit_text = "killed: " + result["killed"]
    elif result["rc"] != 0:
        exit_text = f"the app exited on its own with code {result['rc']}"
    else:
        exit_text = "quit normally (code 0)"
    if not problems:
        print("run_capture: ok (the app quit by itself, target reached)")
    else:
        print("run_capture: FAILED (" + "; ".join(problems) + ")")
    target = ", ".join(result["start"] + result["done"])
    print(f"  exit: {exit_text} | elapsed {result['elapsed']:.1f} s | scene reached: {scene_text} | "
          f"{'target reached' if not result['missing'] else 'target NOT reached'}: {target}")
    if info["capture"]:
        print(f"  capture: {capture} ({capture_note})")
    notes = [l for l in lines if "[E] " in l or "[W] " in l]
    if notes:
        print(f"  [E]/[W] lines ({len(notes)}, showing at most 30):")
        for line in notes[:30]:
            print("    " + line.strip())
        if len(notes) > 30:
            print(f"    ... {len(notes) - 30} more")
    else:
        print("  no [E]/[W] lines")
    return EXIT_OK if not problems else EXIT_FAILED


# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------

FAKE_LIBRARY = {
    "characters": [
        {"id": "model/Sekai Model Pack v4/LeoNeed/Ichika/01_9001.pmx", "displayName": "01_9001"},
        {"id": "model/Other/Miku/Miku.pmx", "displayName": "Miku"},
    ],
    "stages": [
        {"id": "stages:led_stage", "displayName": "LED arena stage"},
        {"id": "MLTD/Aoi Tori", "displayName": "Aoi Tori"},
    ],
    "songs": [
        {"id": "motion/Evergreen", "displayName": "Evergreen", "parts": [{"danceVmd": "a.vmd"}]},
        {"id": "MLTD/Alive Factor v1.1", "displayName": "Alive Factor",
         "parts": [{"danceVmd": "a.vmd"}, {"danceVmd": "b.vmd"}]},
    ],
}


def selftest():
    cases = [
        ("canonical A/B capture",
         ["--character", "01_9001", "--stage", "none", "--song", "Evergreen", "--autoplay", "--seek", "20",
          "--paused", "--no-physics", "--frames", "60", "--effect", "none", "--shader-pack", "none",
          "--capture", "out.png"], True),
        ("bad song name", ["--character", "01_9001", "--stage", "none", "--song", "doesnotexist",
                           "--autoplay", "--frames", "60"], False),
        ("bad character name", ["--character", "nobody", "--song", "Evergreen", "--autoplay", "--frames", "60"],
         False),
        ("bad stage name", ["--character", "01_9001", "--stage", "nostage", "--song", "Evergreen", "--autoplay",
                            "--frames", "60"], False),
        ("autoplay without --song", ["--character", "01_9001", "--autoplay", "--frames", "60"], False),
        ("--frames outside a scene", ["--frames", "60"], False),
        ("lobby preselection only", ["--character", "01_9001", "--song", "Evergreen", "--frames", "60"], False),
        ("play scene without --frames", ["--character", "01_9001", "--song", "Evergreen", "--autoplay"], False),
        ("multi-part song without --formation",
         ["--character", "01_9001", "--song", "Alive Factor", "--autoplay", "--frames", "60"], False),
        ("multi-part song with --formation",
         ["--character", "01_9001", "--song", "Alive Factor", "--formation", "01_9001,Miku", "--autoplay",
          "--frames", "60"], True),
        ("studio ui-script capture",
         ["--character", "01_9001", "--stage", "stages:led_stage", "--screen", "studio", "--ui-script", "s.txt",
          "--frames", "60", "--capture", "ui.png"], True),
        ("studio without --frames", ["--character", "01_9001", "--screen", "studio"], False),
        ("select screen capture", ["--screen", "select", "--frames", "10", "--capture", "ui.png"], True),
        ("offline still from a play scene",
         ["--character", "01_9001", "--song", "Evergreen", "--autoplay", "--seek", "3", "--offline-still",
          "s.png"], True),
        ("offline still without a scene", ["--offline-still", "s.png"], False),
        ("gi render benchmark", ["--benchmark", "dx12-gi-render"], True),
        ("realtime benchmark without --frames", ["--benchmark", "dx12-raster-fhd", "--bench-frames", "600"], False),
        ("realtime benchmark with --frames", ["--benchmark", "dx12-raster-fhd", "--bench-frames", "600",
                                              "--frames", "100000"], True),
        ("--library refused", ["--library", "lib", "--screen", "select", "--frames", "5"], False),
        ("interactive run refused", ["--character", "01_9001", "--song", "Evergreen", "--autoplay"], False),
        ("ui-script without --frames", ["--screen", "studio", "--ui-script", "s.txt"], False),
        ("multi-part song in the studio (no modal there)",
         ["--character", "01_9001", "--song", "Alive Factor", "--screen", "studio", "--frames", "60"], True),
        ("multi-part song in the video dialog",
         ["--character", "01_9001", "--song", "Alive Factor", "--screen", "video", "--offline-video", "v.mp4"],
         False),
        ("unknown --screen", ["--screen", "lobby", "--frames", "10"], False),
        ("unknown --benchmark", ["--benchmark", "dx12-nope", "--frames", "10"], False),
        ("studio project render", ["--project", "p.mmdxproj", "--screen", "studio", "--offline-video", "v.mp4",
                                   "--offline-range", "1", "2"], True),
    ]
    failures = 0
    print(f"run_capture selftest: {len(cases)} argument lists (no launch)")
    for name, args, want_ok in cases:
        issues = check_preflight(args, FAKE_LIBRARY)
        ok = not issues
        if ok != want_ok:
            failures += 1
            print(f"  FAIL {name}: expected {'launch' if want_ok else 'refusal'}, got "
                  f"{'launch' if ok else issues[0]}")
        else:
            print(f"  ok   {name}: {'launch' if ok else 'refused: ' + issues[0]}")

    # --allow-library / --allow-gui relax exactly their own refusal
    allowed = [("--library", "lib", "--screen", "select", "--frames", "5")]
    if check_preflight(*allowed, FAKE_LIBRARY, allow_library=True):
        failures += 1
        print("  FAIL --allow-library still refuses --library")
    else:
        print("  ok   --allow-library permits an isolated library")
    gui = ["--character", "01_9001", "--song", "Evergreen", "--autoplay"]
    if check_preflight(gui, FAKE_LIBRARY, allow_gui=True):
        failures += 1
        print("  FAIL --allow-gui still refuses an interactive run")
    else:
        print("  ok   --allow-gui permits an interactive run")
    if check_preflight(["--frames", "0", "--screen", "studio"], FAKE_LIBRARY, allow_gui=True):
        failures += 1
        print("  FAIL --allow-gui still refuses --frames 0")
    else:
        print("  ok   --allow-gui permits --frames 0 (an interactive run owns its lifetime)")

    # width / height defaults are appended once, never duplicated
    with_defaults = apply_defaults(["--character", "a"])
    if with_defaults != ["--character", "a", "--width", "1600", "--height", "900"]:
        failures += 1
        print(f"  FAIL default window size: {with_defaults}")
    else:
        print("  ok   --width/--height default to 1600 x 900")
    kept = apply_defaults(["--width", "1920", "--height", "1080"])
    if kept != ["--width", "1920", "--height", "1080"]:
        failures += 1
        print(f"  FAIL explicit window size rewritten: {kept}")
    else:
        print("  ok   explicit --width/--height kept")

    print(f"run_capture selftest: {'FAILED' if failures else 'all checks passed'}"
          + (f" ({failures} failure(s))" if failures else ""))
    return EXIT_FAILED if failures else EXIT_OK


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def resolve_exe(value):
    candidates = [value] if value else [DEFAULT_EXE]
    if value and not os.path.isabs(value):
        candidates.append(os.path.join(ROOT, value))
    for candidate in candidates:
        path = os.path.abspath(candidate)
        if os.path.isfile(path):
            return path
    return None


def main(argv):
    # Library ids and windows paths carry CJK text: never let a console code page abort the report.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, OSError, ValueError):
            pass

    # Own options are read up to the first argument that is not ours (or '--'); everything after is the app's
    # own command line, verbatim (so an app flag can never be mistaken for a wrapper flag, with or without '--').
    opts = {"exe": None, "first_frame_timeout": 60.0, "timeout": 120.0, "allow_gui": False,
            "allow_library": False, "selftest": False}
    value_opts = {"--exe": "exe", "--first-frame-timeout": "first_frame_timeout", "--timeout": "timeout"}
    flag_opts = {"--allow-gui": "allow_gui", "--allow-library": "allow_library", "--selftest": "selftest"}
    app_args, i = [], 0
    while i < len(argv):
        tok = argv[i]
        if tok == "--":
            app_args = argv[i + 1:]
            break
        if tok in flag_opts:
            opts[flag_opts[tok]] = True
            i += 1
        elif tok in value_opts:
            if i + 1 >= len(argv):
                print(f"run_capture: {tok} needs a value", file=sys.stderr)
                return EXIT_REFUSED
            try:
                opts[value_opts[tok]] = (float if tok != "--exe" else str)(argv[i + 1])
            except ValueError:
                print(f"run_capture: {tok} wants a number, got '{argv[i + 1]}'", file=sys.stderr)
                return EXIT_REFUSED
            i += 2
        elif tok in ("-h", "--help"):
            print(__doc__.strip())
            return EXIT_OK
        elif tok.startswith("--exe="):
            opts["exe"] = tok.split("=", 1)[1]
            i += 1
        else:
            app_args = argv[i:]          # the app's own arguments start here
            break
    if app_args and app_args[0] == "--":
        app_args = app_args[1:]

    if opts["selftest"]:
        return selftest()

    exe = resolve_exe(opts["exe"])
    if not exe:
        print(f"run_capture: refused: no executable at {opts['exe'] or DEFAULT_EXE} "
              "(another worker may be building; pass --exe build_x/bin/MMDX12.exe)", file=sys.stderr)
        return EXIT_REFUSED

    exe_dir = os.path.dirname(exe)
    if other_app_running(exe):
        print("run_capture: refused: another MMDX12.exe is already running (one window at a time); "
              "wait for it to exit and retry", file=sys.stderr)
        return EXIT_REFUSED

    library, library_error = load_library(exe_dir)
    issues = check_preflight(app_args, library, allow_library=opts["allow_library"],
                             allow_gui=opts["allow_gui"])
    if issues:
        print("run_capture: refused (nothing was launched):", file=sys.stderr)
        for issue in issues:
            print("  - " + issue, file=sys.stderr)
        if library_error and library is None:
            print("  (" + library_error + ")", file=sys.stderr)
        return EXIT_REFUSED

    info = classify(app_args)
    args = apply_defaults(app_args)
    printed = " ".join(f'"{a}"' if " " in a else a for a in args)
    print(f"run_capture: launching {exe} {printed}")
    result = run_app(exe, args, os.path.join(exe_dir, LOG_NAME), opts["first_frame_timeout"], opts["timeout"], info,
                     interactive=opts["allow_gui"])
    return report(result, info)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
