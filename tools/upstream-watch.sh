#!/bin/sh
# Watch the trees this port rides on and say what moved since the pins in CI.
#
#   tools/upstream-watch.sh [--fetch]
#
# The UniXNU pin lives in .github/workflows/firmware-only.yml. The report goes
# to logs/upstream-watch-<date>.md so a later run can be diffed. Nothing is
# merged automatically: the point is to see the gap, and the sync branch is a
# human decision that CI then validates.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=logs/upstream-watch-$(date -u +%Y%m%d-%H%M%S).md
mkdir -p logs

GH=${GH_BIN:-/vol1/1000/项目/hackios/.tools/github-cli/gh_2.103.3_linux_amd64/bin/gh}
GH_HOME=${GH_HOME:-/vol1/1000/项目/hackios/.tools/gh-home}

UNIXNU_PIN=$(sed -n 's/^ *ref: \([0-9a-f]\{40\}\)$/\1/p' .github/workflows/firmware-only.yml | head -1)

if [ "${1:-}" = "--fetch" ]; then
	git fetch --all --prune --quiet
fi

{
	echo "# upstream watch $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo
	echo "## the pins this tree builds against"
	echo
	echo "- UniXNU/UniXNU (firmware-only.yml): \`${UNIXNU_PIN:-missing}\`"
	echo "- local HEAD: \`$(git rev-parse --short HEAD)\` ($(git log -1 --format=%s))"
	echo
	echo "## UniXNU commits since that pin"
	echo
	echo '```'
	if [ -n "$UNIXNU_PIN" ] && git cat-file -e "$UNIXNU_PIN^{commit}" 2>/dev/null; then
		echo "commits behind: $(git rev-list --count "$UNIXNU_PIN..up-unixnu/main" 2>/dev/null || echo '?')"
		git log --oneline --no-decorate "$UNIXNU_PIN..up-unixnu/main" 2>/dev/null | head -60
	else
		echo "(the pinned commit is not in this clone; run with --fetch)"
	fi
	echo '```'
	echo
	echo "## every tree this port rides on (newest commit)"
	echo
	echo '```'
	for repo in UniXNU/UniXNU UniXNU/DarwinBoot Project-Aloha/mu_aloha_platforms \
		ry-diffusion/Bergamota dipa-studio/ios-sweet HeliumStudio-Dev/hackios-xnu-kernel \
		jackyhowh/UniXNU-raphael; do
		printf '%-42s ' "$repo"
		HOME="$GH_HOME" "$GH" api "repos/$repo/commits?per_page=1" \
			--jq '.[0] | "\(.sha[0:8]) \(.commit.author.date) \(.commit.message | split("\n")[0])"' 2>/dev/null ||
			echo "(gh unavailable)"
	done
	echo '```'
	echo
	echo "## the fork this port came from, last 20"
	echo
	echo '```'
	git log --oneline --no-decorate -20 up-jacky/main 2>/dev/null || echo "(up-jacky not fetched)"
	echo '```'
} > "$OUT"

echo "$OUT"
