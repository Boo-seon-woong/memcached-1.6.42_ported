#!/usr/bin/env bash
set -euo pipefail

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-11211}
COUNT=${COUNT:-100}
PREFIX="remote-only-$$-$(date +%s)"
tmp=$(mktemp -d /tmp/memcached-remote-only.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

request() { nc -q 1 "$HOST" "$PORT"; }
stats() { printf 'stats\r\nquit\r\n' | request | tr -d '\r'; }
stat_value() { awk -v key="$1" '$1 == "STAT" && $2 == key { print $3; exit }' "$2"; }
delta() { echo $(( $(stat_value "$1" "$3") - $(stat_value "$1" "$2") )); }

stats >"$tmp/before.txt"
{
    for i in $(seq 1 "$COUNT"); do
        printf 'set %s-%s 0 0 64\r\n0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n' "$PREFIX" "$i"
    done
    printf 'quit\r\n'
} | request >"$tmp/set.txt"
test "$(grep -c '^STORED' "$tmp/set.txt")" -eq "$COUNT"
sleep 1
stats >"$tmp/after-set.txt"
test "$(delta cmd_set "$tmp/before.txt" "$tmp/after-set.txt")" -eq "$COUNT"
test "$(delta curr_items "$tmp/before.txt" "$tmp/after-set.txt")" -eq "$COUNT"
test "$(delta extstore_objects_written "$tmp/before.txt" "$tmp/after-set.txt")" -eq "$COUNT"
test "$(delta extstore_objects_used "$tmp/before.txt" "$tmp/after-set.txt")" -eq "$COUNT"

{
    for i in $(seq 1 "$COUNT"); do
        printf 'set %s-%s 0 0 64\r\nfedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\r\n' "$PREFIX" "$i"
    done
    printf 'quit\r\n'
} | request >"$tmp/overwrite.txt"
test "$(grep -c '^STORED' "$tmp/overwrite.txt")" -eq "$COUNT"
sleep 1
stats >"$tmp/after-overwrite.txt"
test "$(delta cmd_set "$tmp/after-set.txt" "$tmp/after-overwrite.txt")" -eq "$COUNT"
test "$(delta curr_items "$tmp/after-set.txt" "$tmp/after-overwrite.txt")" -eq 0
test "$(delta extstore_objects_written "$tmp/after-set.txt" "$tmp/after-overwrite.txt")" -eq "$COUNT"
test "$(delta extstore_objects_used "$tmp/after-set.txt" "$tmp/after-overwrite.txt")" -eq 0

{
    printf 'get'
    for i in $(seq 1 "$COUNT"); do printf ' %s-%s' "$PREFIX" "$i"; done
    printf '\r\nquit\r\n'
} | request >"$tmp/get.txt"
test "$(grep -c '^VALUE ' "$tmp/get.txt")" -eq "$COUNT"
sleep 1
stats >"$tmp/after-get.txt"
test "$(delta cmd_get "$tmp/after-overwrite.txt" "$tmp/after-get.txt")" -eq "$COUNT"
test "$(delta get_hits "$tmp/after-overwrite.txt" "$tmp/after-get.txt")" -eq "$COUNT"
test "$(delta extstore_objects_read "$tmp/after-overwrite.txt" "$tmp/after-get.txt")" -eq "$COUNT"
test "$(stat_value badcrc_from_extstore "$tmp/after-get.txt")" -eq 0
test "$(stat_value extstore_engine_dead "$tmp/after-get.txt")" -eq 0
test "$(stat_value extstore_read_failures "$tmp/after-get.txt")" -eq 0

{
    for i in $(seq 1 "$COUNT"); do printf 'delete %s-%s\r\n' "$PREFIX" "$i"; done
    printf 'quit\r\n'
} | request >"$tmp/delete.txt"
test "$(grep -c '^DELETED' "$tmp/delete.txt")" -eq "$COUNT"

echo "PASS: SET/overwrite/GET used remote storage only; no local backend or cache controls"
