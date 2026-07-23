#!/bin/bash
set -u
cd /home/seonung/2026/memcached-1.6.42-port
head -c 32 /dev/urandom > /tmp/lentest.key
EXT_CRYPTO_KEY=/tmp/lentest.key EXT_SLOT_SIZE=1024 ./memcached -p 11411 -U 0 -m 512 \
  -o ext_path=local:0:256m,ext_item_size=2,ext_threads=1 >/tmp/lentest.log 2>&1 &
MC=$!; sleep 1.5
N=150
pipe() { exec 3<>/dev/tcp/127.0.0.1/11411; { printf "%b" "$1"; printf "quit\r\n"; } >&3; timeout 5 cat <&3; exec 3<&- 3>&-; }
VA=$(head -c 400 /dev/zero | tr '\0' 'A'); VB=$(head -c 399 /dev/zero | tr '\0' 'B')
# phase 1: 400B sets
S=""; for i in $(seq 1 $N); do S+="set lk$i 0 0 400\r\n$VA\r\n"; done; pipe "$S" >/dev/null
sleep 1
# phase 2: 399B overwrites (frees the 400 slot, reallocs reusing it)
S=""; for i in $(seq 1 $N); do S+="set lk$i 0 0 399\r\n$VB\r\n"; done; pipe "$S" >/dev/null
sleep 1
# phase 3: gets
S=""; for i in $(seq 1 $N); do S+="get lk$i\r\n"; done
OUT=$(pipe "$S")
hits=$(echo "$OUT" | grep -c "VALUE lk[0-9]* 0 399")
echo "=== GET hits (expect $N): $hits ==="
echo "=== stats ==="; pipe "stats\r\n" | grep -iE "badcrc|get_misses|get_hits|cmd_set" | tr -d '\r'
kill $MC 2>/dev/null
