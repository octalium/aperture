---
name: roll-cimgui-wrap
description: Bump the pinned cimgui revision in dep/cimgui.wrap to a newer upstream commit. Use this when adopting an upstream cimgui change (new ImGui API, bug fix, etc.) or as a scheduled wrap maintenance pass. Walks through the pin update, verifies the recurring quirks that have bitten us before (IMGUI_DISABLE_OBSOLETE_FUNCTIONS preservation, ImGuiInputTextFlags_EnterReturnsTrue correctness), and the manual smoke test that catches regressions the build alone won't.
---

`dep/cimgui.wrap` tracks the `docking_inter` branch of our fork at github.com/octalium/cimgui (a mirror of cimgui/cimgui's `docking_inter` — a rolling branch with no upstream tags). We pin a specific commit SHA so builds are reproducible and we can verify the new revision before adopting it. The fork insulates us from upstream history rewrites or branch deletion; upstream sync is manual / on-demand. This skill is the deterministic recipe for bumping that pin.

> Aperture's meson uses `subproject_dir: 'dep'` (set in `meson.build`), so fetched subproject sources land under `dep/<n>/`, not the default `subprojects/<name>/`. The recipe below assumes that layout.

## When to roll

- An upstream cimgui change you want (new ImGui feature, bug fix you've hit, security release).
- Scheduled maintenance pass to avoid the pin going stale.
- A wrap-level defect we've worked around in app code (see #407 history) gets fixed upstream and we want to drop the workaround.

## When NOT to roll

- Mid-feature work that depends on a specific ImGui API. Finish the feature on the current pin first.
- Right before a release. Roll on `dev` so any wrap regression has a buffer before users see it.

## Procedure

### 1. Pick the new SHA

First, fast-forward the fork's `docking_inter` from upstream so the SHA you choose is present on `octalium/cimgui` (the wrap fetches from the fork). On the GitHub UI the fork's "Sync fork" button does this; from the CLI:

```
cd /tmp && git clone https://github.com/octalium/cimgui.git
cd cimgui
git remote add upstream https://github.com/cimgui/cimgui.git
git fetch upstream docking_inter
git checkout docking_inter
git merge --ff-only upstream/docking_inter
git push origin docking_inter
git log --oneline origin/docking_inter | head -20
```

Pick a commit that looks reasonable (avoid SHAs whose commit message reads "WIP" or that change the build system; prefer commits with green CI on cimgui's repo). The HEAD of `docking_inter` is usually fine unless you're chasing a specific upstream change.

### 2. Update `dep/cimgui.wrap`

Edit `dep/cimgui.wrap`. Change the `revision` line to the new SHA, and update the date comment:

```
[wrap-git]
url = https://github.com/octalium/cimgui.git
# pinned to docking_inter on the octalium/cimgui fork as of YYYY-MM-DD.
# the fork mirrors cimgui/cimgui's docking_inter so we own history and are
# insulated from upstream rewrites or branch deletion. upstream sync is
# manual / on-demand: when we want newer upstream commits, fast-forward the
# fork's docking_inter from cimgui/cimgui and bump `revision` here.
revision = <new-sha>
# do not re-add `depth = 1` without rethinking pinning: a shallow clone fetches only
# branch HEAD and silently defeats `revision`, so fresh checkouts drift off the pin.
clone-recursive = true
patch_directory = cimgui
```

Preserve **every other line** verbatim. `patch_directory = cimgui` is load-bearing — it tells meson to apply the patches in `dep/cimgui/` (currently the meson.build that builds cimgui) over the upstream checkout. Never re-add `depth = 1` — it silently defeats the pin (see #502).

### 3. Re-fetch and build

```
rm -rf dep/cimgui            # force a fresh clone (the wrap re-fetches on next setup)
meson setup build --reconfigure
meson compile -C build
```

A clean build is the first sanity check — necessary but **not sufficient**. Plenty of cimgui-wrap regressions compile fine and only show up at runtime (the #407 EnterReturnsTrue bug was exactly that). Don't skip the manual smoke test in step 5.

If compile fails, the most likely culprits are:

- **`IMGUI_DISABLE_OBSOLETE_FUNCTIONS` regression** (recurring per the `cimgui_wrap_rolling` memory). Upstream sometimes adds calls to APIs that this define disables. Either:
  - Bump cimgui forward past the offending commit (upstream usually fixes its own breakage within a few commits), or
  - Patch the call sites in our `dep/cimgui/` patch directory.
- **Generator drift.** cimgui regenerates its C bindings via a Lua generator (`generator/`). If our patches conflict with upstream's regeneration, you'll see merge-conflict-style errors when meson applies patches. Resolve by updating the patch or regenerating against the new upstream.

### 4. Verify `ImGuiInputTextFlags_EnterReturnsTrue` (issue #407 regression check)

cimgui had a bug where `igInputText` with `EnterReturnsTrue` would return true on plain edits, not just on Enter (see #407). The fix pinned at SHA `07fde25e7aff0b2b3eb536dd162d5ead162609d8` — verify the new SHA also has correct behavior:

```
grep -A 5 'EnterReturnsTrue' dep/cimgui/imgui/imgui_widgets.cpp | head
```

You should see `validated` only being set inside the explicit Enter / KeypadEnter / Shift+Enter / gamepad branches — not in a generic edit-completion branch. If `validated` leaks out (returns true on plain edits), the workaround we removed in #411 has to come back. Don't roll onto that SHA.

### 5. Manual smoke test

Run the app and open the Save-as-Pipeline modal (`Pipelines panel → Save as...`). Type characters in the name input. Verify:

- Typing does NOT trigger the Save action (it would close the modal if it did).
- Pressing Enter triggers Save (or shows the overwrite prompt if the name collides).
- Pressing Escape cancels.
- Autofocus lands on the input on open.

If any of those misbehave, the new cimgui revision has a regression — back out.

### 6. Commit

```
git checkout -b chore/<n>-roll-cimgui
git add dep/cimgui.wrap
git commit -m "chore(wrap): roll cimgui to <short-sha>

<one-line summary of what this brings in or fixes>"
```

Conventional commit, no Co-Authored-By trailer per CLAUDE.md. Open a draft PR per the usual flow.

### 7. PR body checklist

In the PR body, document:

- Old SHA → new SHA.
- Why rolling (upstream feature, fix, or scheduled).
- `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` confirmed preserved (link to grep output or commit if you had to patch).
- `EnterReturnsTrue` verified Enter-only (link to the relevant `imgui_widgets.cpp` region in the new SHA).
- Save-as-Pipeline modal smoke-tested.

## If something looks off

If the new SHA introduces issues we don't immediately understand, the safest fallback is rolling back to the prior pin (preserved in git history). Don't ship a half-fixed roll — the wrap is too central. Open an issue documenting the regression and pick a different upstream SHA next pass.

---
*If something in this skill looks wrong, the source code is authoritative. Verify against the current tree and update this skill in the same PR.*
