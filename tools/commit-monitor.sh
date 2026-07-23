#!/usr/bin/env bash
# Minimal ariel-side commit monitor for the memcached-1.6.42_ported channel.
# Polls origin/main; when a non-[ariel] commit appears (i.e. genie replied),
# logs it and records state under .monitor/. Does NOT run any commands.
#
#   SELF=ariel POLL_SECONDS=15 ./tools/commit-monitor.sh
#
# Read genie's latest reply:  cat .monitor/pending_summary.txt ; git pull --ff-only
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STATE="$ROOT/.monitor"; mkdir -p "$STATE"
SELF=${SELF:-ariel}
POLL=${POLL_SECONDS:-15}
log(){ printf '[%s] %s\n' "$(date -u '+%F %T UTC')" "$*" | tee -a "$STATE/monitor.log" >&2; }

handled=$(cat "$STATE/handled" 2>/dev/null || git -C "$ROOT" rev-parse origin/main)
echo "$handled" > "$STATE/handled"
log "monitor start SELF=$SELF poll=${POLL}s handled=$handled"

while true; do
  if git -C "$ROOT" fetch -q origin main 2>/dev/null; then
    tip=$(git -C "$ROOT" rev-parse origin/main)
    if [ "$tip" != "$handled" ]; then
      subj=$(git -C "$ROOT" log --format='%h %s' "$handled..$tip")
      # flag only if some commit in the range is NOT ours
      if printf '%s\n' "$subj" | grep -qvE "^[0-9a-f]+ \[$SELF\]"; then
        log "NEW remote commit(s):"
        printf '%s\n' "$subj" | tee -a "$STATE/monitor.log" >&2
        printf '%s\n' "$tip" > "$STATE/pending_wake"
        printf '%s\n' "$subj" > "$STATE/pending_summary.txt"
      fi
      handled=$tip; echo "$handled" > "$STATE/handled"
    fi
  else
    log "fetch failed"
  fi
  sleep "$POLL"
done
