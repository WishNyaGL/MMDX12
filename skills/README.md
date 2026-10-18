# MMDX12 agent skills

These skills let an AI agent direct a Studio project through MMDX12's MCP server: camera work with
`mmdx12-camera-director`, lighting with `mmdx12-lighting-director` and motion editing with
`mmdx12-motion-editor`. The agent decides what the shot, cue or edit should be; the app's tools compute the frames,
solve the camera position and write the keys. No scripts, no VMD parsing, no repository paths are involved.

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
