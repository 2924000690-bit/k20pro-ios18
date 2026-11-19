#!/bin/sh
# Unattended UniXNU sync: when the upstream tip moves past what CI pins, make a
# branch that bumps the pin, push it, and let the firmware-only pipeline say
# whether our patches still apply. One branch per upstream commit; a branch that
# already exists is not rebuilt. Log: logs/upstream-autosync.log
#
# cron: 17 */6 * * *  (installed in /etc/cron.d/hackios-upstream)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
LOG=logs/upstream-autosync.log
mkdir -p logs

GH=${GH_BIN:-/vol1/1000/项目/hackios/.tools/github-cli/gh_2.103.3_linux_amd64/bin/gh}
GH_HOME=${GH_HOME:-/vol1/1000/项目/hackios/.tools/gh-home}
REPO=Gsjsjzhznsz/UniXNU-raphael-iOS18-K20Pro
WF=.github/workflows/firmware-only.yml

say() { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >> "$LOG"; }

say "check start"
git fetch up-unixnu --quiet 2>/dev/null || { say "fetch failed"; exit 0; }
PIN=$(sed -n 's/^ *ref: \([0-9a-f]\{40\}\)$/\1/p' "$WF" | head -1)
TIP=$(git rev-parse up-unixnu/main 2>/dev/null || true)
[ -n "$PIN" ] && [ -n "$TIP" ] || { say "no pin or tip"; exit 0; }
SHORT=$(git rev-parse --short=8 "$TIP")
SHORTPIN=$(echo "$PIN" | cut -c1-12)
say "pin=$SHORTPIN tip=$SHORT"
[ "$PIN" = "$TIP" ] && { say "up to date"; exit 0; }

BRANCH="upstream-sync-$SHORT"
if git ls-remote --exit-code --heads origin "$BRANCH" >/dev/null 2>&1; then
	say "branch $BRANCH already exists; nothing to do"
	exit 0
fi
git diff --quiet -- "$WF" || { say "workflow file has local edits; skip"; exit 0; }
git diff --quiet --cached -- "$WF" || { say "workflow file staged; skip"; exit 0; }

git checkout -b "$BRANCH" main >/dev/null 2>&1 || { say "checkout failed"; exit 0; }
sed -i "s|^          ref: .*$|          ref: $TIP|" "$WF"
git add "$WF"
git commit -q -m "ci: try the UniXNU tip ($SHORT) in the firmware pipeline

Automatic upstream sync: the pin in this file moved from $PIN to $TIP."
if ! git push -u origin "$BRANCH" >/dev/null 2>&1; then
	say "push failed"
	git checkout main >/dev/null 2>&1 || true
	exit 0
fi
KERNEL_RUN=$(HOME="$GH_HOME" "$GH" run list -R "$REPO" --workflow xnu.yml --status success \
	--limit 1 --json databaseId --jq '.[0].databaseId' 2>/dev/null || echo "")
if [ -n "$KERNEL_RUN" ]; then
	HOME="$GH_HOME" "$GH" workflow run firmware-only.yml --ref "$BRANCH" -R "$REPO" \
		-f kernel_run_id="$KERNEL_RUN" -f variant=full -f device=xiaomi-raphael >/dev/null 2>&1 &&
		say "pushed $BRANCH and dispatched firmware-only with kernel run $KERNEL_RUN" ||
		say "pushed $BRANCH but dispatch failed"
else
	say "pushed $BRANCH; no kernel run id to dispatch with"
fi
git checkout main >/dev/null 2>&1 || true
say "check done"
