# Moonlight V+ PC — handoff

This is the state of the work on **AsafMah/moonlight-qt** (the user's fork of qiin2333/moonlight-qt, which is itself a fork of moonlight-stream/moonlight-qt), as of 2026-10-24. It covers what is done, how the branches fit together, how to set up a new machine, and what is next.

> To a Claude session picking this up: read this whole file, then `tools/AGENT-CONVENTIONS.md`. The roadmap with the original mockups is the artifact "Moonlight V+ PC Roadmap": https://claude.ai/artifact/LfyiZdYcnqfEHf3C5E13e9

## 1. Where things stand

**The latest good build** is CI run [40076964501](https://github.com/AsafMah/moonlight-qt/actions/runs/40076964501), on `integration` @ `4c97349b`. Every job passed on every platform. Download the `Moonlight-VPlus-Portable-x64-*` artifact from that run.

### Done (all in `integration`, all on the fork)

| Item | Branch(es) | What it does |
|---|---|---|
| S1 shortcuts | `up/shortcuts` → `feat/shortcuts` | A customizable shortcut registry. Bindings match by scancode (keysym first for letters and digits), with exact modifiers. Includes a settings card. |
| O1+O2 stats overlay | `up/stats` → `feat/stats-overlay` | A typed `StreamStatsModel`/`StatsSnapshot` and a movable overlay window with presets (minimal/compact/detailed/custom). New metrics: lows, jitter, latency breakdown and FEC. |
| X2 session summary | `up/session-summary` → `feat/session-summary` | An optional per-window CSV recorder, a summary card after each stream, and a Session level in the menu. |
| M2 host actions | `feat/host-actions` | Foundation Sunshine Super Commands ("Host Commands"), a Send Keys level, and up to 12 custom key macros. |
| M3 stream quality | `feat/live-stream-settings` | Changes resolution and FPS mid-session by reconnecting in place, and bitrate live. `Session::tryApplyStreamQualityLive()` is a placeholder for a live path once PR #104 and a supporting host exist. |
| M1 tabbed menu | `feat/menu-tabs` | The in-stream menu, split into tabs: Quick, Display, Input, Host and Session. Rows are placed by `kTabAssignments`, levels are found by `openAction`, and it works with gamepad LB/RB, keyboard and mouse. It also merges M3 and X2 and adapts them to the tabs. |
| PW-0/1 | `feat/pyrowave` | PyroWave runtime/build scripts, the settings panel and host capability detection. The runtime is skipped on macOS and Windows ARM64, because Granite has no SIMD code there. |
| PW-2 | `feat/pyrowave-decoder` | PyroWave SDR decode and present, sharing one Vulkan device between libplacebo and PyroWave. |
| PW-3 | `feat/pyrowave-hdr` | PyroWave HDR10 and HLG, with HDR metadata. |

**PRs opened elsewhere** (at the user's request):
- [qiin2333/moonlight-vplus#754](https://github.com/qiin2333/moonlight-vplus/pull/754): English strings for the Android client. It's small, because the app was already almost fully translated.
- [AlkaidLab/foundation-sunshine#1180](https://github.com/AlkaidLab/foundation-sunshine/pull/1180): English default names for the menu commands. These were the Chinese "触摸键盘/桌宠" items shown in the Android game menu.

**moonlight-common-c:** AsafMah/moonlight-common-c, branch `feat/client-introspection` @ `dbb0adb`, cut from qiin `mic` @ 24f2369. It adds:
- `LiGetHostPyrowaveCapabilities()` + `LI_PYROWAVE_HAS_HOST_CAPABILITIES_QUERY`;
- `LiGetRTPVideoFecStats()` + `LI_HAS_RTP_VIDEO_FEC_STATS`.

It is **not** yet the submodule pin. The app code is already guarded with `#ifdef`s, so bumping the pin turns both features on.

### Not verified anywhere yet
- Nothing has been tried against a real host.
- The UI has barely been looked at on screen. Only the Shortcuts settings card was checked by hand.
- PyroWave has never run on a GPU.

## 2. Next tasks, in order

The user asked for 1 to 3 together. Three agents were started for them, but they were cut off by usage limits before they had changed anything, so they start from scratch. The full briefs are in §6.

1. **Performance overlay: free resizing, responsive layout and tooltips** (`feat/stats-overlay`). User report: in the Detailed preset almost every field name is cut off. Needed:
   - resizing by dragging edges and corners;
   - a layout that adapts to the size, so no label is ever clipped;
   - a fixed default Detailed size;
   - hover tooltips with metric explanations whenever the mouse is not captured.
2. **Hold to release the mouse** (core on `up/shortcuts`, the rest on `feat/shortcuts`). For example, hold Right Alt and the mouse is free; release it and the mouse is captured again. This needs press/release "hold" actions, bare-modifier bindings for hold actions, and AltGr handling. Afterwards, wire its "mouse released" state to the overlay tooltips.
3. **More in-stream menu options** (`feat/menu-tabs`):
   - positions: edges, corners, side panel, remember a dragged position;
   - layouts: compact, comfortable, side panel; scale and opacity;
   - triggers: gamepad combo choice, floating button on/off, hold-to-peek, an optional screen-edge trigger, Esc closes vs. goes back.

   Defaults must keep today's behaviour exactly.
4. **Detachable performance overlay:** its own top-level window that can move to another screen. The user asked for this "later", after 1–3. Exclusive fullscreen on the same monitor will cover it, so say so in the setting.
5. **PW-4: PyroWave stats and capability page.**
   - Frame pacing.
   - Triple buffering after delayed presents.
   - The SDR CPU-staging fallback.
   - Share the stats formatter with the overlay.
   - Translate PW-2's strings.
   - Bump the common-c submodule to `feat/client-introspection`.
   - Have the capability page show the chosen GPU and the preflight result.
6. **A4: rebase PR qiin2333/moonlight-qt#104** (local `asaf-104`, which tracks `origin/feat/client-driven-resolution`). It's blocked on common-c #10/#13 and foundation-sunshine #729. Once that lands, wire M3's `tryApplyStreamQualityLive()` to `LiSendResolutionChangeRequest` when the host advertises `LI_FF_CLIENT_RESOLUTION_CHANGE`.
7. **A smoke test on screen with the user:** the tabbed menu, the overlay, the stream-quality switch and the summary card.
8. **Upstream PRs** from the `up/*` branches. Never open these without asking the user.

**Open questions for the user**, still unanswered from Track B (PyroWave):
- Fallback codec: H.264, or Automatic?
- A toast instead of a dialog?
- Naming: "Foundation Sunshine" in the UI?
- Should locked controls be greyed out?

Optional: V1 VRR (roadmap §07; compare with qiin2333/moonlight-qt#10). This hasn't been picked yet.

## 3. Branch model

- **Remotes:**
  - `origin` = AsafMah/moonlight-qt
  - `qiin` = qiin2333/moonlight-qt
  - `upstream` = moonlight-stream/moonlight-qt
- **`base/up`** (ec71a4f5) is the merge-base with upstream. `up/*` branches start from it and must build against stock upstream. Upstream PRs come from these.
- **`base/qiin`** (e9655464): `feat/*` branches start from it. A `feat/*` branch **merges** its `up/*` counterpart, never rebases.
- **`integration`** = `qiin/master` + the branches in `tools/branches.txt`, in order. It is disposable: `tools/rebuild-integration.sh` rebuilds it and you force-push it. It relies on **git rerere**, which remembers how each merge conflict was resolved, to replay those resolutions; see §4.
- **Rule for the in-stream menu** (`overlaymenupanel.*`): add levels and items and look them up by `MenuAction` or `openAction`, never by a fixed index or a y-offset in tests. Assign tabs only through `kTabAssignments`.
- **Rule for CI:** each feature branch adds its own test lines to `.github/workflows/build.yml`, after a different existing test line, so the branches merge without conflicts.
- **Identity:** the repo-local git identity is `Asaf Mahlev <mahlev.asaf@gmail.com>`; set it with `git config user.name/user.email` in each clone.
  - Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - PR bodies end with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **What not to push:** don't push to qiin2333 or moonlight-stream, and don't open PRs there, without asking. Pushing `integration` (force), `feat/*` and `up/*` to the fork is approved.

## 4. Setting up a new Windows machine

Use short paths: the scratch and long paths broke git and Win32 tools before. The scripts in `tools/` hard-code `E:\Code\...`, `E:\Qt\...` and a Python Scripts path. Edit those at the top of each script, or use the same layout.

1. **Clone:**
   ```bash
   git clone https://github.com/AsafMah/moonlight-qt E:/Code/moonlight-qt
   cd E:/Code/moonlight-qt
   git remote add qiin https://github.com/qiin2333/moonlight-qt
   git remote add upstream https://github.com/moonlight-stream/moonlight-qt
   git fetch --all
   git config user.name "Asaf Mahlev"; git config user.email mahlev.asaf@gmail.com
   git config rerere.enabled true
   for b in base/up base/qiin integration $(git branch -r | grep -oE 'origin/(feat|up)/[^ ]+' | sed 's|origin/||'); do git branch -f $b origin/$b; done
   git checkout integration && git submodule update --init --recursive
   ```
2. **The dev-tools branch:** `git worktree add E:/Code/dev-tools dev-tools`, then copy `tools/*` to `E:\Code\moonlight-tools\`.
3. **Restore the rerere cache.** Without it, every rebuild of `integration` has to resolve about 20 conflicts by hand.
   ```bash
   tar -C E:/Code/moonlight-qt/.git -xzf E:/Code/dev-tools/rr-cache.tar.gz
   ```
4. **Toolchain** (the same as CI):
   - VS 2022 Build Tools, with the C++ workload, the Windows SDK and CMake.
   - Qt **6.12.0 msvc2022_64**. Install it with **naqt** (the tool CI uses); aqtinstall couldn't find 6.12.0. Install to `E:\Qt\6.12.0\msvc2022_64`.
   - jom, in `E:\Qt\Tools\jom`.
   - clang-format **21.1.8**: `pip install clang-format==21.1.8`. CI's `cpp-format` job checks the changed lines against the merge-base with `origin/master`.
   - `E:\Qt\Tools\shim\7z.exe`: compile `shim/7z.c` with `cl 7z.c`. This no-op lets `scripts\build-arch.bat` finish locally without 7-Zip. Alternatively, install 7-Zip.
   - Prebuilt `libs/`: `setup-deps.ps1` downloads them. Use `-PyroWave Require` to also build the PyroWave runtime DLL; local builds didn't include it before.
5. **Build and test:**
   - Build: `cmd //c "E:\\Code\\moonlight-tools\\build-local.bat release [checkout]"`. A final "Build failed" right after `[7z shim] skipped` is expected.
   - The output lands in `build\deploy-x64-release`, and `Moonlight.msi` is in `build\build-x64-release`.
   - Unit tests: `cmd //c "E:\\Code\\moonlight-tools\\test-local.bat <dir> <target> [checkout]"`.
   - It sets `QT_QPA_PLATFORM=offscreen`, so `overlay_button_native_wake` fails there. That's expected: run it without offscreen and it passes.
   - CI's full test list is in the `run-test.bat` lines of `build.yml`.
6. **CI:** `gh workflow run build.yml -R AsafMah/moonlight-qt --ref integration`.
   - The workflow has `cancel-in-progress: false`, but a *new pending* run still replaces an older pending one.
   - The portable artifact is named `Moonlight-VPlus-Portable-x64-*`.

### Gotchas that cost time before
- **Python output on Windows:** `python -I` ignores `PYTHONIOENCODING`, so use `sys.stdout.reconfigure(encoding='utf-8')` when printing CJK text.
- **Backslashes in heredocs:** in the agent's Bash tool, `\\` inside a heredoc sometimes reached Python as a single `\`. Use `chr(92)`, or edit with the Edit tool.
- **Chinese .ts merges:** keep each message in its original `<context>`. A plain keep-both merge can move messages into the wrong context.
- **The menu navigation test** used to click by pixel offset. It now finds rows by action; keep it that way.
- **Agent worktrees:** create them by hand (`git worktree add .claude/worktrees/<n> <branch>`), plus a `libs` junction: `New-Item -ItemType Junction -Path <wt>\libs -Target E:\Code\moonlight-qt\libs`.

## 5. Other repositories on the old machine

They're all pushed, so there's nothing to copy:
- `E:\Code\moonlight-common-c` → AsafMah/moonlight-common-c, `feat/client-introspection`.
- `E:\Code\moonlight-vplus` → AsafMah/moonlight-vplus, `i18n/english-strings` (PR #754).
- `E:\Code\foundation-sunshine` → AsafMah/foundation-sunshine, `i18n/default-menu-cmd-names` (PR #1180).

## 6. Briefs for the three interrupted tasks

Each brief assumes the agent reads `tools/AGENT-CONVENTIONS.md`. Use the worktree for that branch.

### 6.1 Performance overlay: resizing, responsive layout, tooltips (`feat/stats-overlay`)

**Files:**
- `app/streaming/video/overlaystats{window,controller,preset}.*`
- `app/settings/statsoverlaysettings.*`
- `tests/stats_overlay_layout/`
- the settings card in `DisplaySettingsPage.qml`

**1. Free-form resizing.**
- In the existing edit/move mode (drag moves, wheel scales, Ctrl+wheel changes opacity), dragging the edges and corners resizes the overlay to any size, within minimum and maximum limits.
- Save the size per display under `statsoverlay/displays/<displayKey>/...`.

**2. Responsive layout.** Labels must never be clipped. As space runs out, in order:
1. Use more columns.
2. Show "label: value" on one line, or put the label above the value when narrow.
3. Use a short form of the label before truncating.
4. Scale the graphs.
5. Drop the lowest-priority metrics, with a "+N" marker.

Fix the Detailed preset's default size so every label is readable at 100% at 1080p and 4K. Make the layout a pure function of (metrics, size, font metrics) that returns the layout. Test it, including "no label clipped at the Detailed default size".

**3. Hover tooltips.**
- Show them whenever the mouse is not captured: in edit mode, after the user releases the pointer, and with the hold-to-release key.
- Each tooltip has the full name, a one- or two-sentence explanation of what the metric means and what good or bad looks like, and the current value with units.
- Keep click-through while the stream has the mouse.
- The overlay is a QPainter raster window, so draw the tooltip in that style.
- Translate into zh_CN and zh_TW.

**4. Settings card.** Add "Reset size" and show the current size.

**Plan ahead:** keep the layout and painting independent of the stream window, so task 4 (detaching) only changes the window parent and flags.

**Don't touch** `overlaymenupanel.*`, or `shortcuts.*` beyond what's needed.

### 6.2 Hold to release the mouse (`up/shortcuts` core → `feat/shortcuts`)

**Hold actions.**
- Add actions that fire on press *and* release, and the action `hold-release-mouse` ("Hold to release mouse").
- It is off by default with no default binding; suggest Right Alt in the card.
- Hold actions can be bound to a bare modifier. Left and right modifiers must be told apart by scancode.

**Key handling.**
- The bound key's press and release never reach the host.
- Pick sensible behaviour for other keys pressed while it's held, and document the choice.
- Restore mouse capture correctly on focus loss or when the menu opens. Recapture only if the mouse was captured when the hold started.

**AltGr.** Handle AltGr, which SDL and Windows report as LCtrl+RAlt. The fake LCtrl must not break matching or reach the host. Add a test.

**Where the code goes.** The generic press/release semantics and their tests go on `up/shortcuts`. The fork-specific parts go on `feat/shortcuts`, after merging `up/shortcuts` into it.

**Expose the "mouse released" state** so the overlay tooltips (6.1) can observe it. **Don't touch** `overlaymenupanel.*` or `overlaystats*`.

### 6.3 In-stream menu options (`feat/menu-tabs`)

**Starting point.** Look at what exists first:
- the Menu Position level;
- the overlay button (`overlaybutton*`);
- the menu settings in QML.

**Positions.**
- Centered, at the cursor, each edge (full-height left or right side panel) and each corner.
- Remember a dragged position. The title bar can be dragged while the mouse is free.
- Stay on-screen with multiple monitors and DPI scaling.

**Layouts.**
- Compact (current), Comfortable (bigger rows, for TV or gamepad), and Side panel (tabs as a vertical rail).
- Scale 75–150% and opacity.
- Titles must never be truncated; wrap or widen instead.

**Triggers.**
- The existing shortcut.
- A choice of gamepad combo, if that's simple.
- The floating button on or off.
- Optional "hold the shortcut to peek, release to close".
- Optional screen-edge/corner trigger while the mouse is free, with a delay, off by default.
- Esc/Back either closes the menu or goes up one level.

**Settings.** Every option goes in the settings UI and, where it makes sense, in a Display-tab level. Save them under `overlaymenu/...`. Defaults must keep today's behaviour exactly.

**Shortcuts.** If a trigger needs a new shortcut action, keep the change to `shortcuts.*` minimal and list it in your report.

**Tests.** Extend `overlay_menu_navigation`:
- the geometry stays on-screen;
- no title is truncated in any layout;
- side-panel rail navigation works with gamepad and keyboard;
- the edge-trigger logic, written as a pure function.

**Don't touch** `overlaystats*`.

### After all three
1. Wire the hold-to-release state (6.2) into the overlay tooltips (6.1).
2. Merge each branch's tests into its CI lines.
3. Rebuild `integration`, build it, run every CI test locally, and run the format gate. Then force-push and run CI.
