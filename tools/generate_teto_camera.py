import sys
import os
import glob
import struct
import numpy as np

sys.path.insert(0, r"C:\Users\admin\.gemini\config\skills\mmdx12-camera-director\scripts")
from camera_engine import DirectorCameraEngine, write_camera_vmd, solve_solo_frame, BEZ_FLOAT, BEZ_SMOOTH_S, BEZ_FLOW

TOOLS_DIR = r"E:\repos\MMDX12\tools"
if os.path.exists(TOOLS_DIR) and TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)
from mcp_pipe import MmdxPipe

def build_teto_tracks():
    vmd_path = r'C:\Users\admin\Documents\mmdx12projects\테토\teto-overdrive - Sour式重音テトUTAU (Sour Teto Kasane) [Sour Kasane Teto UTAU-tail.pmx].vmd'
    TOTAL_FRAMES = 4345

    tracks_raw = {}
    with open(vmd_path, 'rb') as f:
        f.seek(50)
        cnt = struct.unpack('<I', f.read(4))[0]
        for i in range(cnt):
            raw = f.read(15).split(b'\x00')[0]
            try: name = raw.decode('shift_jis')
            except: name = ''
            frame = struct.unpack('<I', f.read(4))[0]
            pos = struct.unpack('<3f', f.read(12))
            rot = struct.unpack('<4f', f.read(16))
            f.seek(64, 1)
            if name not in tracks_raw: tracks_raw[name] = {}
            tracks_raw[name][frame] = (pos, rot)

    def interpolate_track(bone_name, default_pos):
        pos_arr = np.zeros((TOTAL_FRAMES, 3), dtype=np.float64)
        if bone_name not in tracks_raw:
            pos_arr[:] = default_pos
            return pos_arr
        kfs = sorted(tracks_raw[bone_name].items(), key=lambda x: x[0])
        frames = [k[0] for k in kfs]
        positions = np.array([k[1][0] for k in kfs], dtype=np.float64)
        for ch in range(3):
            pos_arr[:, ch] = np.interp(np.arange(TOTAL_FRAMES), frames, positions[:, ch])
        return pos_arr

    c_pos = interpolate_track('センター', [0.0, 0.0, 0.0])
    lh_pos = interpolate_track('左手首', [-3.0, 10.0, 0.0])
    rh_pos = interpolate_track('右手首', [3.0, 10.0, 0.0])
    lik_pos = interpolate_track('左足ＩＫ', [-1.0, 0.0, 0.0])
    rik_pos = interpolate_track('右足ＩＫ', [1.0, 0.0, 0.0])

    pelvis = c_pos + np.array([0.0, 9.5, 0.0])
    chest  = c_pos + np.array([0.0, 12.8, 0.0])
    head   = c_pos + np.array([0.0, 15.5, 0.0])
    eyes   = c_pos + np.array([0.0, 16.2, 0.0])
    ear_top= c_pos + np.array([0.0, 17.8, 0.0])
    foot   = 0.5 * (lik_pos + rik_pos)

    tracks = {
        1: {
            'name': 'Teto',
            'pelvis': pelvis,
            'chest': chest,
            'head': head,
            'eyes': eyes,
            'ear_top': ear_top,
            'foot': foot,
            'l_hand': lh_pos,
            'r_hand': rh_pos,
        }
    }
    return tracks

def main():
    print("Building dancer kinematics tracks...")
    tracks = build_teto_tracks()
    aspect = 16.0 / 9.0
    engine = DirectorCameraEngine(tracks, aspect=aspect)
    mid = 1

    # Definition of the 7 Continuous Cinematic Takes
    # Each take is a list of waypoints: (frame, mode, rx, ry, rz, fov, pan_x, pan_y, curve, aperture)
    # Between waypoints in the SAME take, the camera glides smoothly with ZERO cuts!
    # Hard cuts occur ONLY between takes, with >= 35 deg angle contrast or extreme scale contrast!

    takes = [
        # --- TAKE 1: Intro (0 .. 494) [Sweeping Steadicam approach, ZERO CUTS] ---
        [
            (0,   'FS',   2.0,   0.0, 0.0, 34, 0.0, -0.4, BEZ_FLOAT,    0.45),
            (160, 'MS',   0.0,   3.0, 0.0, 26, 0.0, -0.2, BEZ_SMOOTH_S, 0.60),
            (320, 'MCU', -2.0,  -6.0, 0.0, 19, 0.0,  0.1, BEZ_SMOOTH_S, 0.72),
            (494, 'MCU',  1.0,  -8.0, 0.0, 16, 0.0,  0.2, BEZ_SMOOTH_S, 0.78),
        ],

        # --- TAKE 2: Verse 1 (495 .. 906) [45 deg Profile Entrance -> Orbit Tracking, ZERO CUTS] ---
        # HARD CUT @ 495: Jump to dramatic 45 deg stage-left profile CU (Angle delta = 34 deg, FOV 16 -> 14)
        [
            (495, 'CU',  -1.0, -42.0, 3.0, 14, 0.0,  0.2, BEZ_SMOOTH_S, 0.82),
            (640, 'MS',   2.0, -22.0, 1.0, 21, 0.0, -0.2, BEZ_FLOW,     0.70),
            (780, 'MCU',  3.0, -10.0, 0.0, 17, 0.0,  0.1, BEZ_SMOOTH_S, 0.75),
            (906, 'MCU',  0.0,  -2.0, 0.0, 18, 0.0,  0.1, BEZ_SMOOTH_S, 0.75),
        ],

        # --- TAKE 3: Pre-Chorus 1 (907 .. 1236) [Opposite Low Hero Angle -> Drop Riser, ZERO CUTS] ---
        # HARD CUT @ 907: Jump to opposite stage-right low angle HERO (Angle delta = +38 deg, Scale MCU -> HERO)
        [
            (907,  'HERO', -7.0,  36.0, 0.0, 24, 0.0, -0.4, BEZ_SMOOTH_S, 0.68),
            (1020, 'HERO', -5.0,  20.0, 0.0, 22, 0.0, -0.3, BEZ_FLOW,     0.70),
            (1130, 'MCU',   1.0,   8.0, 0.0, 20, 0.0,  0.1, BEZ_SMOOTH_S, 0.75),
            (1236, 'FS',   -2.0,   0.0, 0.0, 33, 0.0, -0.5, BEZ_SMOOTH_S, 0.45),
        ],

        # --- TAKE 4: Chorus 1 Drop! (1237 .. 1730) [Wide Drop -> Optical Push to Killing Part, ZERO CUTS] ---
        # HARD CUT @ 1237: Instant Beat 1 smash cut to Wide Stage Floor Reveal (Scale FS, low angle rx = -8)
        [
            (1237, 'FS',  -8.0,   0.0,  0.0, 30, 0.0, -0.5, BEZ_SMOOTH_S, 0.55),
            (1380, 'MS',   2.0,   6.0,  0.0, 22, 0.0, -0.2, BEZ_FLOW,     0.70),
            (1520, 'MCU',  1.0,  -6.0, -2.0, 17, 0.0,  0.1, BEZ_SMOOTH_S, 0.78),
            (1650, 'CU',   2.0, -12.0,  0.0, 12, 0.0,  0.2, BEZ_SMOOTH_S, 0.85),
            (1730, 'CU',   1.0, -10.0,  0.0, 13, 0.0,  0.2, BEZ_SMOOTH_S, 0.85),
        ],

        # --- TAKE 5: Dance Break & Verse 2 (1731 .. 2389) [Dutch Hero Cut -> Righting Roll, ZERO CUTS] ---
        # HARD CUT @ 1731: Cut to aggressive Dutch low angle (rx = -10, rz = +7, ry = -28)
        [
            (1731, 'HERO', -10.0, -28.0, 7.0, 24, 0.0, -0.4, BEZ_SMOOTH_S, 0.65),
            (1880, 'MS',    -2.0, -14.0, 0.0, 22, 0.0, -0.2, BEZ_FLOW,     0.70),
            (2050, 'MCU',    1.0,  -4.0, 0.0, 17, 0.0,  0.1, BEZ_SMOOTH_S, 0.78),
            (2220, 'MCU',    3.0,   8.0, 0.0, 16, 0.0,  0.1, BEZ_SMOOTH_S, 0.80),
            (2389, 'MCU',    0.0,   0.0, 0.0, 18, 0.0,  0.1, BEZ_SMOOTH_S, 0.75),
        ],

        # --- TAKE 6: Pre-Chorus 2 & Bridge (2390 .. 3624) [Intimate Bokeh Profile -> Grand Riser, ZERO CUTS] ---
        # HARD CUT @ 2390: Cut to stage-right intimate portrait (Angle delta = +42 deg, CU, FOV 18 -> 13)
        [
            (2390, 'CU',   2.0,  42.0, 0.0, 13, 0.0,  0.2, BEZ_FLOAT,    0.85),
            (2650, 'MCU',  1.0,  20.0, 0.0, 16, 0.0,  0.1, BEZ_FLOW,     0.80),
            (2900, 'ECU',  2.0,   6.0, 0.0, 11, 0.0,  0.3, BEZ_SMOOTH_S, 0.88),
            (3250, 'CU',   0.0, -15.0, 0.0, 14, 0.0,  0.2, BEZ_FLOAT,    0.82),
            (3480, 'MS',   0.0,  -4.0, 0.0, 22, 0.0, -0.2, BEZ_SMOOTH_S, 0.65),
            (3624, 'FS',  -3.0,   0.0, 0.0, 32, 0.0, -0.5, BEZ_SMOOTH_S, 0.45),
        ],

        # --- TAKE 7: Final Chorus & Outro (3625 .. 4340) [Grand Climax -> Majestic Rise, ZERO CUTS] ---
        # HARD CUT @ 3625: Final climax smash cut to Wide Stage Reveal (Scale FS, low angle rx = -8)
        [
            (3625, 'FS',  -8.0,  0.0,  0.0, 31, 0.0, -0.5, BEZ_SMOOTH_S, 0.55),
            (3800, 'MS',   2.0,  8.0,  0.0, 20, 0.0, -0.2, BEZ_FLOW,     0.70),
            (3950, 'CU',   3.0, -8.0,  0.0, 13, 0.0,  0.2, BEZ_SMOOTH_S, 0.85),
            (4118, 'MS',  -1.0,  0.0,  0.0, 22, 0.0, -0.2, BEZ_FLOAT,    0.65),
            (4340, 'FS',  -7.0,  0.0,  0.0, 27, 0.0, -0.6, BEZ_FLOAT,    0.50),
        ],
    ]

    print(f"Generating {len(takes)} continuous cinematic takes (Total 6 intentional high-contrast cuts)...")
    for take_idx, waypoints in enumerate(takes):
        for wp in waypoints:
            f, mode, rx, ry, rz, fov, px, py, curve, ap = wp
            _, *s = solve_solo_frame(tracks, f, mid, mode, rx, ry, rz, fov,
                                     pan_x=px, pan_y=py, aspect=aspect)
            engine._add_kf(f, s, curve)
            engine._add_focus(f, mid, ap, "head" if mode in ('CU', 'ECU', 'MCU') else "upper")

    out_dir = r"C:\Users\admin\Documents\mmdx12projects\테토"
    vmd_cam_path = os.path.join(out_dir, "teto_camera_smooth.vmd")
    print(f"Writing {len(engine.kfs)} keyframes to {vmd_cam_path}...")
    write_camera_vmd(vmd_cam_path, engine.kfs)

    # Connect to MCP
    app = MmdxPipe()

    # Clean rollback of previous camera keys
    print("Rolling back previous camera/focus keys...")
    while True:
        st = app.call("studio_get_state")
        u = st.get("undo", "")
        if u == "focus key":
            app.call("undo")
        elif "카메라" in u or "camera" in u.lower():
            app.call("undo")
            break
        else:
            break

    # Inject smooth Camera VMD
    print(f"Injecting smooth camera VMD: {vmd_cam_path}")
    res = app.call("studio_command", cmd="studioimport", args=[os.path.abspath(vmd_cam_path)])
    print(f"Camera import result: {res}")

    # Inject Focus Keyframes
    print(f"Injecting {len(engine.focus_kfs)} DoF focus keyframes...")
    for (f, mode, f_mid, ap, offset, bone) in engine.focus_kfs:
        app.call("studio_command", cmd="studiofocus",
                 args=[str(f), mode, str(f_mid), f"{ap:.2f}", str(offset), bone])

    # Save Project
    st_proj = app.call("studio_get_state")
    proj_path = st_proj.get("project_path")
    res_save = app.call("studio_save", file=proj_path)
    print(f"Project saved: {res_save}")

    # Capture Verification Screenshots
    print("Capturing verification screenshots...")
    shot_dir = os.path.join(out_dir, "smooth_shots")
    os.makedirs(shot_dir, exist_ok=True)
    check_frames = [100, 495, 907, 1237, 1650, 1731, 2390, 2900, 3625, 4200]
    for cf in check_frames:
        app.call("seek", seconds=cf / 30.0)
        app.call("wait_frames", n=15)
        png = os.path.join(shot_dir, f"smooth_{cf:04d}.png")
        app.call("screenshot", path=png, scene_only=True)
        print(f"  Frame {cf:4d} captured -> {png}")

    print("Smooth camera direction complete!")

if __name__ == '__main__':
    main()
