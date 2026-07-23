#!/usr/bin/env bash
# ariel-side event watcher for the memcached-1.6.42_ported channel.
# Polls origin/main and EXITS 0 as soon as a non-[ariel] commit (a genie result
# report, an admin question, or a work request) appears, after recording it.
# The agent is woken by that exit, handles the entry (pull -> respond in
# conversation.md -> push), then re-arms this watcher. Idle-polls otherwise.
#
#   SELF=ariel POLL_SECONDS=30 ./tools/commit-monitor.sh
#
# After waking, read the trigger:  cat .monitor/pending_summary.txt
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STATE="$ROOT/.monitor"; mkdir -p "$STATE"
SELF=${SELF:-ariel}
POLL=${POLL_SECONDS:-30}
log(){ printf '[%s] %s\n' "$(date -u '+%F %T UTC')" "$*" | tee -a "$STATE/monitor.log" >&2; }

handled=$(cat "$STATE/handled" 2>/dev/null || git -C "$ROOT" rev-parse origin/main)
echo "$handled" > "$STATE/handled"
log "watch start SELF=$SELF poll=${POLL}s handled=$handled"

while true; do
  if git -C "$ROOT" fetch -q origin main 2>/dev/null; then
    tip=$(git -C "$ROOT" rev-parse origin/main)
    if [ "$tip" != "$handled" ]; then
      subj=$(git -C "$ROOT" log --format='%h %s' "$handled..$tip")
      if printf '%s\n' "$subj" | grep -qvE "^[0-9a-f]+ \[$SELF\]"; then
        printf '%s\n' "$tip"  > "$STATE/pending_wake"
        printf '%s\n' "$subj" > "$STATE/pending_summary.txt"
        log "EVENT: non-$SELF commit(s) — waking agent:"
        printf '%s\n' "$subj" | tee -a "$STATE/monitor.log" >&2
        exit 0
      fi
      handled=$tip; echo "$handled" > "$STATE/handled"   # our own commits: skip
    fi
  else
    log "fetch failed"
  fi
  sleep "$POLL"
done
