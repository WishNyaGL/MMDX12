# mme_compat_run.py

M7a compatibility harness (`docs/mme/mme_design.md` §4/§6, M7). It runs the MMDX12 MME runtime against the
downloaded effect pack in `captures/mme_lib/` and writes the compatibility report. It uses the extra
`build_mme` binaries (`mme_parse_test.exe` with `--summary`, `mme_compile_test.exe`, `MMDX12.exe`) and needs
numpy + Pillow for the image metrics and the contact sheet. The pack is untrusted third-party content, so
run the script with `python -I` (isolated mode); it never executes anything from inside `captures/mme_lib`.

## Stages

1. **Discover** — for every `_status.json` entry with `status: ok`, the `.fx` files in its `src` tree that no
   other `.fx` there `#include`s are the entry points; each is classified with `mme_parse_test --summary`
   (kind, technique `MMDPass` list, offscreen use, diagnostic counts — the flag was added for this tool).
   The **primary** entry point is the one named like the entry's folder / id or readme-mentioned, else the
   first post-kind file, else the first object-kind file; the rest are listed as **variants**.
2. **Static** (no window) — `mme_compile_test` over the primary entry points (object mode for object
   techniques); the pass rate per effect and the first error line.
3. **Render** (one app window per effect, strictly sequential, `timeout 180` each) — the pinned scene
   `--character pda_miku/head --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics
   --frames 60 --effect none --shader-pack none --dof 0 --render raster --upscaler none --width 1280
   --height 720`, plus one baseline capture without MME. Post → `--mme <fx>`, object → `--mme-model <fx>`,
   mixed → both. Captures go to `captures/mme_compat/<rank>_<id>.png`, the log is copied next to it.
   Automatic signals per run: exit code / crash (`[E] CRASH`), `[E]`/`[W]` lines, `MME LOAD ... programs=a/b`,
   `MME FRAME`/`MME OBJECT` (did it draw), and image metrics vs the baseline (mean abs diff, % pixels
   changed, fully black / white / flat detection).
4. **Report** — `captures/mme_compat/results.json`, `docs/mme/compat_list.md` (table: rank, name, kind,
   static pass rate, drew?, image delta, auto tier, notes, capture link) and a contact sheet
   `captures/mme_compat/sheet.png` (thumbnails with name + tier). The auto tier guess is: X = crash / no
   LOAD / 0 programs; C = loaded but did not draw or black/white frame or < 50 % programs; B = drew with
   warnings or partial programs; A = drew, all programs, no `[E]`. The architect sets the final tier in
   `docs/mme/compat_overrides.json` (`{"<rank>": {"final_tier": "A", "notes": "..."}}`); the script preserves
   those columns across re-runs.

## Usage

```
python -I tools/mme/mme_compat_run.py [--only 13,70,6] [--resume] [--limit N] [--all-variants]
                                      [--static-only] [--no-static] [--sheet-only]
```

- `--only`: comma-separated ranks or ids (default: all ok entries).
- `--resume`: skip entries that already have a render result.
- `--limit N`: only the first N selected entries.
- `--all-variants`: also render the non-primary entry points.
- `--static-only`: the static stage only, no app windows.
- `--no-static`: render only (keeps earlier static results).
- `--sheet-only`: regenerate the report and contact sheet from `results.json`, no runs.

Results are merged across runs, so a `--static-only` pass followed by an `--only ...` render pass keeps both
halves. `build_mme/bin/mmdx12.ini` is backed up and restored around the render batch, and the script waits
for any running `MMDX12.exe` before each run (never two instances).

# mme_scan.py

Standalone Python 3 (stdlib only) research tool that scans a folder tree of MikuMikuEffect (MME)
`.fx` effects and reports, per effect file, which MME features it uses and whether its shaders
compile:

- (a) as a D3D9 effect: `fxc /T fx_2_0`
- (b) per pass as Shader Model 5: `fxc /T vs_5_0|ps_5_0 /Gec`

It never modifies the scanned files and is not part of the app build.

## Usage

```
python tools/mme/mme_scan.py <root> [<root>...] --out <dir> [--fxc <path>] [--no-compile] [--jobs N] [--limit N]
```

- `--out` (required): output directory. Writes `scan.json` (full data), `scan.csv` (one row per
  effect file) and `summary.md` (feature frequency table, compile pass rates, top error messages,
  unknown semantics/annotations). Temporary fxc inputs go to `<out>/tmp` and are cleaned at the end.
- `--fxc`: path to `fxc.exe`. Default: newest `C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe`.
- `--no-compile`: feature extraction only, no fxc calls.
- `--jobs N`: parallel fxc processes (default: CPU count). Each fxc call has a 60 s timeout.
- `--limit N`: only process the first N effect files.

## Examples

```
# full scan
python tools/mme/mme_scan.py library --out captures/mme_scan_library --jobs 8

# features only, first 20 effects
python tools/mme/mme_scan.py library --out captures/mme_scan_library --no-compile --limit 20
```

## Notes

- Effect entries are `*.fx` (case-insensitive). `*.fxsub`, `*.fxh`, `*.hlsl`, `*.h` are include-only:
  they are followed through `#include` of an `.fx`, never scanned as entries.
- Include paths and `ResourceName` textures resolve relative to the including file, then to the
  effect's folder, case-insensitively (mirroring `src/asset/MmeHints.cpp`).
- Text decoding: UTF-8 (BOM stripped), else cp932, else latin-1; the guess is recorded. For the
  `fx_2_0` test (a) the original bytes' file is passed, for the SM5 test (b) a temporary copy is
  written in the same encoding.
- Parsing is regex + brace matching on comment-stripped text (no full HLSL parser).
- A bad file never crashes the run: every per-file failure becomes a record with `error`.
