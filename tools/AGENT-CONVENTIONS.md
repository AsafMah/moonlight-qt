# Conventions for agents working on the Moonlight V+ PC fork

Repo: E:\Code\moonlight-qt (Qt 6 / C++ / QML; fork of qiin2333/moonlight-qt, itself a fork of moonlight-stream/moonlight-qt).
Each agent has its own git worktree under `E:\Code\moonlight-qt\.claude\worktrees\<name>`. Work ONLY there.
`libs` is a junction to the shared prebuilt libs, so don't delete it.

## Branches
- `base/up` = merge-base with upstream moonlight-stream (ec71a4f5). `up/*` branches must build against stock upstream.
- `base/qiin` = qiin/master base for the fork. `feat/*` branches are fork features; they *merge* (never rebase) their `up/*` counterpart.
- `integration` = qiin/master + all feat branches. It is disposable and maintained by the orchestrator, so don't touch it.
- Existing feature branches: feat/shortcuts (S1 shortcut registry, `app/streaming/input/shortcuts.*`), feat/stats-overlay (`app/streaming/stats/*` StreamStatsModel/StatsSnapshot, overlay window), feat/host-actions (Send Keys / Host Commands menu levels), feat/pyrowave-decoder (PW-2).
- In-stream menu: `app/streaming/video/overlaymenupanel.{h,cpp}`. Other branches APPEND levels/items and look them up by `MenuAction`, never by fixed index. Keep that pattern so parallel branches merge with "keep both sides".

## Commits
- Identity is repo-local (Asaf Mahlev). Conventional commits (`feat(scope): ...`), small and focused.
- Every message ends with a blank line then `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do NOT push, and do not open PRs. The orchestrator pushes.
- Before finishing, format the changed lines:
  `PATH="/c/Users/lureb/AppData/Roaming/Python/Python314/Scripts:$PATH" git clang-format --extensions c,cpp,h,mm <base>` (base/qiin for feat/*, base/up for up/*). Commit the result as `style(...)`.

## Build and test (Windows, from bash)
- Full app: `cmd //c "E:\\Code\\moonlight-tools\\build-local.bat release E:\\Code\\moonlight-qt\\.claude\\worktrees\\<name>" > <log> 2>&1`
  - Then grep the log for ` error |fatal error|LNK[0-9]+:`.
  - A trailing "Build failed" right after `[7z shim] skipped` is expected and fine.
  - The worktree needs its submodules: `git submodule update --init --recursive` (pyrowave has update=none; init it with `git -c submodule.third-party/pyrowave.update=checkout submodule update --init --recursive third-party/pyrowave` only if your branch contains it).
- Unit tests (qmake projects under tests/<dir>): `cmd //c "E:\\Code\\moonlight-tools\\test-local.bat <dir> <target> E:\\Code\\moonlight-qt\\.claude\\worktrees\\<name>"`
- Other agents build in parallel, so the machine is busy. Builds are slow; run them in the background and don't spin.
- QML: run qmllint where possible. The app can't be visually checked by you, so say what is unverified.
- Translations: add new user-visible strings to `app/languages/qml_zh_CN.ts` and `qml_zh_TW.ts` if the branch's neighbours do (they do); write real Chinese translations.

## Report back (final message)
- The commits, with hash and subject.
- The files touched.
- Design decisions and anything that differs from the brief.
- Build and test results.
- `git merge-tree --write-tree` results of your branch against `qiin/master` and `integration`, listing any conflicting files.
- Everything unverified.
