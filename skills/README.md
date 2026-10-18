# MMDX12 agent skills

These skills are for creative direction, not presets. They teach an AI agent to work the way a director does:
listen to the song and the performance, commit to a treatment, storyboard with intent, use the app's tools as a crew
(camera solving, key writing, contact sheets), then look honestly at the result and revise. `mmdx12-camera-director`
covers camera work, `mmdx12-lighting-director` covers stage lighting and `mmdx12-motion-editor` humanizes stiff
motion as an animator would;
each carries a treatment template, a critique rubric and tool recipes as references so the judgement stays the core
and the procedure stays a means. No scripts, no VMD parsing, no repository paths are involved.

## Requirements

- MMDX12 is running with MCP enabled (Detail Settings > MCP Control, or launched with `--mcp`).
- A Studio project is open. `studio_get_state` must answer with a studio screen and a project; the analysis tools
  reject when no project is open.
- The MCP bridge `mmdx12_mcp.exe` is registered in your agent. It ships next to `MMDX12.exe` in the release zip;
  the setup steps are in `MCP_GUIDE.md` in the same zip (the source document is `docs/mcp.md`).
- A song loaded in the project for beat/section analysis. Without audio the skills fall back to the motion's timing.

## Install

Copy the skill folder(s) into your agent's skills directory:

- Claude Code: `~/.claude/skills/` (for example `~/.claude/skills/mmdx12-camera-director/SKILL.md`)
- Gemini / Antigravity: `~/.gemini/skills/` or `~/.agents/skills/`
- Codex: `~/.codex/skills/`

Each folder is self-contained: `SKILL.md` plus its `references/` files. Install one or more.
