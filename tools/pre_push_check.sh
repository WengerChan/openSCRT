#!/usr/bin/env bash
# The checkpoint for the rule "do not push to main while the CI is red".
#
# The rule was written down in AGENTS.md and then broken six times in a row on
# 2026-10-25: a hot-fix chain went onto main while the macOS leg was red, and none of
# those commits said why that was safe. A rule without a checkpoint is a wish, so here is
# the checkpoint.
#
# It asks GitHub what the last real run said and then:
#
#   * green                 -> the push may go;
#   * red, or still running -> the push has to carry the reason, and this script looks
#                             for it in the commit message;
#   * no token, no jq       -> it says so and stops. It does not guess.
#
# Usage:
#   tools/pre_push_check.sh            check HEAD, the commit that is about to be pushed
#   tools/pre_push_check.sh <sha>      check a commit
#
# Exit codes: 0 the push may go, 1 it may not (or the check could not be made).

set -euo pipefail

REPO="${OPENCRT_REPO:-WengerChan/openSCRT}"
# The sentence a push has to carry when the CI is not green. Both spellings are accepted:
# one for the English commit messages, one for the Chinese ones.
MARKER_EN="Why this push is safe"
MARKER_ZH="为什么安全"

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

# The network goes through the proxy the repository uses.
if [ -f ./.proxy.sh ]; then
    # shellcheck disable=SC1091
    . ./.proxy.sh
fi

commit="${1:-HEAD}"
message="$(git log -1 --format=%B "$commit" 2>/dev/null || true)"

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is needed to read the run" >&2
    exit 1
fi

token=""
if credential="$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill 2>/dev/null)"; then
    token="$(printf '%s\n' "$credential" | sed -n 's/^password=//p')"
fi
if [ -z "$token" ]; then
    echo "no GitHub token available: git credential fill gave nothing" >&2
    echo "look at the last run by hand, or set the credential up, and come back" >&2
    exit 1
fi

run="$(curl -s -H "Authorization: Bearer $token" \
    "https://api.github.com/repos/$REPO/actions/runs?per_page=1")"
read -r run_id status conclusion sha <<EOF
$(printf '%s\n' "$run" | jq -r '.workflow_runs[0] | "\(.id) \(.status) \(.conclusion) \(.head_sha)"')
EOF

if [ -z "$run_id" ] || [ "$run_id" = "null" ]; then
    echo "the run list came back empty, so the state of the CI is unknown" >&2
    exit 1
fi

echo "last run: $run_id  $status/$conclusion  $sha"

# The decision goes into the repository, so that "the checkpoint was consulted" is
# something a reader can see rather than something somebody claims. A push that is not
# in this table did not go through the checkpoint -- the gap is the evidence.
record_decision() {
    local decision="$1"
    printf '| %s | %s | %s %s/%s | %s |\n' "$(date '+%Y-%m-%d %H:%M')" \
        "$(git rev-parse --short "$commit" 2>/dev/null || echo "$commit")" \
        "$run_id" "$status" "$conclusion" "$decision" >> docs/push-log.md
    echo "recorded in docs/push-log.md (it rides along with the next commit)"
}

if [ "$status" = "completed" ] && [ "$conclusion" = "success" ]; then
    echo "the last run is green: the push may go"
    record_decision "绿，放行"
    exit 0
fi

echo "the last run is not green, so the push has to say why it is safe." >&2
echo "Put this sentence in the commit message:" >&2
echo "  $MARKER_EN" >&2
echo "  ($MARKER_ZH)" >&2
if printf '%s\n' "$message" | grep -qE "$MARKER_EN|$MARKER_ZH"; then
    echo "the message of $commit carries it:"
    printf '%s\n' "$message" | grep -E "$MARKER_EN|$MARKER_ZH"
    record_decision "非绿，提交信息里有理由"
    exit 0
fi
echo "the message of $commit does not carry it, so the push may not go" >&2
exit 1
