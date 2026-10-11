#!/usr/bin/env python3
"""Headless end-to-end test for the delta (smart) updater.

Imitates the 2026-10-06 (2) auto-update E2E, but exercises the delta path:

  * build an "old" install = a copy of dist/MMDX12-1.9.1-win64 with the newly built
    build/bin/MMDX12.exe dropped in, plus a dummy library file, an ini and a recovery file;
  * build a mock "new" version 9.9.9 from that tree (one changed shader, one new file, one
    removed file in a subfolder, a VERSION.txt change); run tools/make_update.py on it into a
    local object store and also zip it (for the full fallback); write a local feed json;
  * scenario 1: run the old exe --update-feed <feed> --ui-script <updateinstall>,
    wait for the applier + the relaunched app to quit, then assert the delta install;
  * scenario 2: corrupt one object in the store, re-run from a fresh old install and assert it
    falls back to the zip and still installs correctly.

Run:
    python tools/update_e2e.py [--keep]

Requires a prior `build.cmd build` (build/bin/MMDX12.exe) and dist/MMDX12-1.9.1-win64.
Standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(os.environ.get("TEMP", os.environ.get("TMP", ".")), "opencode", "delta_e2e")
DIST = os.path.join(ROOT, "dist", "MMDX12-1.9.1-win64")
EXE = os.path.join(ROOT, "build", "bin", "MMDX12.exe")
MAKE_UPDATE = os.path.join(ROOT, "tools", "make_update.py")
OLD_VER = "1.9.1"
NEW_VER = "9.9.9"

failures = []


def log(msg):
    print(msg, flush=True)


def check(cond, msg):
    if cond:
        log(f"  OK   {msg}")
    else:
        log(f"  FAIL {msg}")
        failures.append(msg)
    return cond


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def force_dirs_ok(path):
    """rmtree that tolerates read-only files (a git checkout can leave them)."""
    def onerror(func, p, exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except Exception:
            pass
    if os.path.exists(path):
        shutil.rmtree(path, onerror=onerror)


def make_old_install(dest):
    """A copy of the 1.9.1 dist with the new exe, plus user data that must survive."""
    force_dirs_ok(dest)
    shutil.copytree(DIST, dest)
    # Runtime leftovers that must never be part of the manifest / must stay untouched.
    for d in ("shader_cache", "cache"):
        force_dirs_ok(os.path.join(dest, d))
    for f in os.listdir(dest):
        if f.endswith(".log") or f.lower() == "mmdx12.ini":
            os.remove(os.path.join(dest, f))
    # The new exe (the one under test), as an install would already have a version of it.
    shutil.copyfile(EXE, os.path.join(dest, "MMDX12.exe"))
    # User data that the swap must leave alone.
    with open(os.path.join(dest, "mmdx12.ini"), "w", encoding="utf-8") as f:
        f.write("nickname=TestUser\nupdateFeedUrl=\n")
    with open(os.path.join(dest, "dummy_library_file.txt"), "w", encoding="utf-8") as f:
        f.write("user library content\n")
    os.makedirs(os.path.join(dest, "recovery"), exist_ok=True)
    with open(os.path.join(dest, "recovery", "recover.bin"), "wb") as f:
        f.write(b"recovery data")
    # A file the manifest will not list (removed version 9.9.9): must vanish after the swap.
    os.makedirs(os.path.join(dest, "shaders", "old"), exist_ok=True)
    with open(os.path.join(dest, "shaders", "old", "gone.hlsl"), "w", encoding="utf-8") as f:
        f.write("// removed in 9.9.9\n")
    os.makedirs(os.path.join(dest, "library"), exist_ok=True)
    with open(os.path.join(dest, "library", "user_model.txt"), "w", encoding="utf-8") as f:
        f.write("keep me\n")
    with open(os.path.join(dest, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write(OLD_VER + "\n")
    return dest


def make_new_tree(old, dest):
    """The mock 9.9.9 tree: one changed shader, one new file, one removed file, VERSION.txt."""
    force_dirs_ok(dest)
    shutil.copytree(old, dest)
    for d in ("shader_cache", "cache", "update_tmp", "recovery"):
        force_dirs_ok(os.path.join(dest, d))
    for f in os.listdir(dest):
        if f.endswith(".log") or f.lower() == "mmdx12.ini":
            os.remove(os.path.join(dest, f))
    # Change one shader (append a line so its hash changes).
    with open(os.path.join(dest, "shaders", "mmd.hlsl"), "a", encoding="utf-8") as f:
        f.write("\n// 9.9.9 marker\n")
    # New file.
    with open(os.path.join(dest, "NEW_IN_999.txt"), "w", encoding="utf-8") as f:
        f.write("brand new in 9.9.9\n")
    # Removed file in a subfolder.
    os.remove(os.path.join(dest, "shaders", "old", "gone.hlsl"))
    os.rmdir(os.path.join(dest, "shaders", "old"))
    # VERSION.txt change.
    with open(os.path.join(dest, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write(NEW_VER + "\n")
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="do not delete the temp workspace")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        sys.exit(f"build/bin/MMDX12.exe missing - run build.cmd build first")
    if not os.path.isdir(DIST):
        sys.exit(f"{DIST} missing")
    python = sys.executable

    force_dirs_ok(TMP)
    os.makedirs(TMP, exist_ok=True)
    store = os.path.join(TMP, "store")
    os.makedirs(store, exist_ok=True)

    log("== building old install + new 9.9.9 tree ==")
    old = make_old_install(os.path.join(TMP, "old_1.9.1"))
    newtree = make_new_tree(old, os.path.join(TMP, "new_9.9.9"))

    # make_update.py on the new tree -> local manifest + object store. No --base-url: the
    # manifest's objects field stays "../objects/", resolved relative to the manifest file.
    res = run([python, MAKE_UPDATE, newtree, "--version", NEW_VER, "--out", store])
    log(res.stdout.strip())
    if res.returncode != 0:
        sys.exit("make_update.py failed")
    manifest_path = os.path.join(store, "manifests", NEW_VER + ".json")
    manifest_sha = sha256_file(manifest_path)

    # Full zip (fallback path) + feed.
    zip_path = os.path.join(TMP, f"MMDX12-{NEW_VER}-win64.zip")
    log("== zipping the full fallback ==")
    parent = os.path.dirname(newtree)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, dns, fns in os.walk(newtree):
            for fn in fns:
                full = os.path.join(dp, fn)
                z.write(full, os.path.join(os.path.basename(newtree), os.path.relpath(full, newtree)))
    zip_size = os.path.getsize(zip_path)
    zip_sha = sha256_file(zip_path)

    feed = {
        "version": NEW_VER,
        "url": zip_path.replace("\\", "/"),
        "size": zip_size,
        "sha256": zip_sha,
        "page": "https://mmdx.codingbot.kr/releases/",
        "manifest": manifest_path.replace("\\", "/"),
        "manifestSha256": manifest_sha,
        "notes": {"ko": "9.9.9 테스트", "en": "9.9.9 test"},
    }
    feed_path = os.path.join(TMP, "latest.json")
    with open(feed_path, "w", encoding="utf-8") as f:
        json.dump(feed, f, ensure_ascii=False, indent=1)

    log(f"  full zip: {zip_size} bytes; manifest: {manifest_path}")

    ok1 = scenario(1, old, newtree, feed_path, store, expect_delta=True)
    # Scenario 2 needs a fresh old install (scenario 1 installed 9.9.9 into it) and a corrupted
    # object in a *copy* of the store (keep the good store for the manifest itself).
    old2 = make_old_install(os.path.join(TMP, "old_1.9.1_b"))
    ok2 = scenario(2, old2, newtree, feed_path, store, expect_delta=False, corrupt=True)

    log("")
    if failures:
        log(f"FAILED ({len(failures)}):")
        for f in failures:
            log("  - " + f)
    else:
        log("ALL CHECKS PASSED")
    if args.keep:
        log(f"workdir kept: {TMP}")
    return 1 if failures else 0


def scenario(n, old, newtree, feed_path, store, expect_delta, corrupt=False, changed=3):
    log(f"\n== scenario {n} ({'delta' if expect_delta else 'fallback'}) ==")
    work = old  # the app runs from the old install dir
    store2 = store
    if corrupt:
        store2 = os.path.join(TMP, f"store_corrupt_{n}")
        force_dirs_ok(store2)
        shutil.copytree(store, store2)
        # Corrupt one object: the changed shader's object (pick any file's hash from the manifest).
        with open(os.path.join(store, "manifests", NEW_VER + ".json"), encoding="utf-8") as f:
            m = json.load(f)
        # Corrupt the object of shaders/mmd.hlsl specifically.
        target = next(f["sha256"] for f in m["files"] if f["path"] == "shaders/mmd.hlsl")
        obj = os.path.join(store2, "objects", target[:2], target)
        with open(obj, "r+b") as f:
            f.write(b"CORRUPT!")            # same size kept? if not, size mismatch triggers anyway
        # Rebuild the feed pointing the manifest at the corrupted store's manifest copy.
        shutil.copyfile(os.path.join(store, "manifests", NEW_VER + ".json"),
                        os.path.join(store2, "manifests", NEW_VER + ".json"))
        manifest2 = os.path.join(store2, "manifests", NEW_VER + ".json")
        with open(feed_path, encoding="utf-8") as f:
            feed = json.load(f)
        feed["manifest"] = manifest2.replace("\\", "/")
        feed["manifestSha256"] = sha256_file(manifest2)
        # Point the object store at the corrupted copy: make_update wrote "../objects/" relative to
        # the manifest, so the manifest copy in store2/manifests already resolves to store2/objects.
        feed_path2 = os.path.join(TMP, f"latest_corrupt_{n}.json")
        with open(feed_path2, "w", encoding="utf-8") as f:
            json.dump(feed, f, ensure_ascii=False, indent=1)
        feed_path = feed_path2

    script = os.path.join(TMP, f"update_{n}.script")
    with open(script, "w", encoding="utf-8") as f:
        f.write("10 updateinstall\n")

    log_path = os.path.join(work, "mmdx12.log")
    if os.path.exists(log_path):
        os.remove(log_path)

    cmd = [os.path.join(work, "MMDX12.exe"), "--update-feed", feed_path,
           "--ui-script", script, "--screen", "select", "--update-relaunch-frames", "60"]
    log("  launching: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Sample the updater's log while it runs: the pre-update log is archived on success, and the
    # relaunched app overwrites mmdx12.log, so read the live file before both.
    sampled = []
    deadline = time.time() + 180
    while proc.poll() is None and time.time() < deadline:
        for line in read(os.path.join(work, "mmdx12.log")).splitlines():
            if line not in sampled:
                sampled.append(line)
        time.sleep(0.4)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        check(False, "the updating app did not exit")
        proc.wait()
        return False
    # One immediate final read: the last lines (the delta result) may have been written between the
    # loop's last poll and the exit. The applier archives mmdx12.log to mmdx12.previous.log right
    # after, so poll both for a few seconds until the delta lines are in.
    for _ in range(40):
        for name in ("mmdx12.log", "mmdx12.previous.log"):
            for line in read(os.path.join(work, name)).splitlines():
                if line not in sampled:
                    sampled.append(line)
        if any("update: delta" in s or "falling back to the full zip" in s for s in sampled):
            break
        time.sleep(0.25)
    check(proc.returncode is not None, "updating app exited")
    log("  updater exited, waiting for the applier + relaunch...")
    # The applier writes applied.txt then relaunches; poll for it.
    applied = os.path.join(work, "update_tmp", "applied.txt")
    deadline = time.time() + 120
    while time.time() < deadline and not os.path.exists(applied):
        time.sleep(1)
    # The relaunched app starts fresh (shaders compile ~20 s), logs the breadcrumb and quits with
    # --frames. Poll the fresh log for its "quit after" line, then keep sampling the delta lines.
    log_path = os.path.join(work, "mmdx12.log")
    deadline = time.time() + 150
    while time.time() < deadline:
        txt = read(log_path)
        if "quit after" in txt:
            break
        time.sleep(1)
    time.sleep(1)
    # Pick up the rest of the relaunched app's log (and any lines after the last sample).
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            for line in f.read().splitlines():
                if line not in sampled:
                    sampled.append(line)
    except FileNotFoundError:
        pass
    # CleanupUpdateLeftovers removes update_tmp on the relaunched start; wait for it.
    deadline = time.time() + 30
    while time.time() < deadline and os.path.exists(os.path.join(work, "update_tmp")):
        time.sleep(1)

    # The final log: after the swap the applier archived the pre-update log to mmdx12.previous.log,
    # so the relaunched app wrote a fresh mmdx12.log. Combine the sampled pre-update log (which has
    # the delta lines), any .previous log and the final log.
    logtext = "\n".join(sampled)
    for name in ("mmdx12.previous.log", "mmdx12.log"):
        p = os.path.join(work, name)
        if os.path.exists(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                logtext += f"\n--- {name} ---\n" + f.read()
    if n == 1:
        with open(os.path.join(TMP, "scenario1.log"), "w", encoding="utf-8") as f:
            f.write(logtext)

    if expect_delta:
        check(f"update: delta {changed} files changed" in logtext, f"{changed} files changed, delta path used")
        check("update: delta staged" in logtext, "'update: delta staged' logged")
        check("update: delta failed" not in logtext, "no fallback to zip in the delta run")
    else:
        check("update: delta failed" in logtext and "falling back to the full zip" in logtext,
              "delta failure fell back to the full zip")

    # Installed state.
    top = work  # the swap replaces top-level entries under the install root
    v = read(os.path.join(top, "VERSION.txt"))
    check(v.strip() == NEW_VER, f"VERSION.txt is {NEW_VER} (got {v.strip()!r})")
    check(os.path.exists(os.path.join(top, "NEW_IN_999.txt")), "NEW_IN_999.txt landed")
    mmd = read(os.path.join(top, "shaders", "mmd.hlsl"))
    check("9.9.9 marker" in mmd, "changed shader installed")
    check(not os.path.exists(os.path.join(top, "shaders", "old", "gone.hlsl")),
          "removed file is gone")
    check(os.path.exists(os.path.join(top, "shaders", "common.hlsli")),
          "unchanged file intact (shaders/common.hlsli)")
    check("nickname=TestUser" in read(os.path.join(top, "mmdx12.ini")), "mmdx12.ini intact")
    check(os.path.exists(os.path.join(top, "dummy_library_file.txt")), "dummy library file intact")
    check(os.path.exists(os.path.join(top, "recovery", "recover.bin")), "recovery file intact")
    check(os.path.exists(os.path.join(top, "library", "user_model.txt")), "library/ intact")
    check(not os.path.exists(os.path.join(top, "update_tmp")), "update_tmp cleaned")
    prev = read(os.path.join(top, "mmdx12.previous.log"))
    check("staged, spawning the applier" in prev and "installed - restarting" in prev,
          "mmdx12.previous.log keeps the updating app's + applier's log")
    # The breadcrumb is logged by the restarted app on its next start.
    check("installed (this run started after the update)" in logtext,
          "'update: 9.9.9 installed' breadcrumb logged")

    return not failures


def read(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


if __name__ == "__main__":
    sys.exit(main())
