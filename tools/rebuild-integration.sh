#!/usr/bin/env bash
# Rebuild the personal "integration" branch: qiin/master plus every branch
# listed in branches.txt, merged in order. The integration branch is
# disposable — never open a PR from it, never commit to it by hand.
# git rerere (enabled in the repo) replays conflict resolutions you made before.
set -euo pipefail

repo=${REPO:-/e/Code/moonlight-qt}
list="$(cd "$(dirname "$0")" && pwd)/branches.txt"
target=integration

cd "$repo"
if [ -n "$(git status --porcelain --ignore-submodules)" ]; then
    echo "Working tree has uncommitted changes; commit or stash first." >&2
    exit 1
fi

git fetch -q qiin
git fetch -q origin
git checkout -q -B "$target" qiin/master

while read -r branch; do
    [[ -z "$branch" || "$branch" == \#* ]] && continue
    echo "== merging $branch"
    if ! git merge --no-ff --no-edit "$branch"; then
        if [ -z "$(git diff --name-only --diff-filter=U)" ]; then
            # rerere resolved every conflict from a recorded resolution
            git commit --no-edit
        else
            echo "Conflict merging $branch. Resolve it, commit, then rerun this script;" >&2
            echo "rerere will remember the resolution next time." >&2
            exit 1
        fi
    fi
done < "$list"

git submodule update --init --recursive
echo "integration = qiin/master + $(grep -cv '^\s*\(#\|$\)' "$list") branches"
