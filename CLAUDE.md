# aperture — Claude Code project instructions

## Branch model

- `main` is **spec-only**. Never edit `main`, never branch from `main`, never target PRs at `main`.
- `dev` is the integration branch. All `feat/<n>` and `fix/<n>` branches branch off `dev` and merge back to `dev`.
- Create the `feat/<n>` branch from `dev` **before** editing anything, every time.

## GitHub workflow

- Use plain `gh` and `git`. Do **not** use the loom MCP.
- All work is issue-driven. File an issue first; reference it from the branch name (`feat/<n>-...`) and the PR body (`Closes #<n>`).
- Never write `Co-Authored-By` trailers in commits.

## Team mode

This repo ships an agent-team scaffold under `.claude/agents/`. When the human says **"spin up the team"**, **"set up the team"**, **"team mode"**, or anything equivalent, do this:

1. Call `TeamCreate` with `team_name: aperture`, `agent_type: coordinator`.
2. Spawn teammates with the `Agent` tool, all on the `aperture` team:
   - one teammate named `pr-reviewer` using the `pr-reviewer` subagent type
   - three teammates named `worker-1`, `worker-2`, `worker-3` using the `worker` subagent type
3. You (the calling session) **become the coordinator** — follow `.claude/agents/coordinator.md`.
4. List current open issues with `gh issue list` and wait for the human to direct dispatch.

Each teammate reads its own definition file at spawn time, so per-role rules live in `.claude/agents/<role>.md`. Keep this file's "Team mode" section in sync with the agent files when you add or rename roles.
