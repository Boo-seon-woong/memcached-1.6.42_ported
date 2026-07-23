# memcached-1.6.42_ported — ariel/genie deployment channel

Fresh coordination channel for the extstore **RDMA remote-memory** port. This
repo IS the code; genie builds directly from it. Append-only log.

Convention:
- Each entry ends with exactly one `NEXT: genie` / `NEXT: ariel` / `NEXT: none` / `NEXT: admin`.
- Commit prefixes: `[ariel]` / `[genie]` / `[admin]`. Pull before writing.
- Report commands as fenced code blocks with their key output (not just prose).
- **ariel runs a persistent watcher and replies to every result report, admin
  question, or work request.** When ariel needs an approval/permission decision
  (anything risky, destructive, or outside the agreed plan), it asks here with
  `NEXT: admin` instead of blocking on an interactive prompt. Approval requests
  are kept to a minimum.

Roles:
- **ariel** — SEV guest / non-TEE host; runs memcached (KVS compute + RDMA client).
- **genie** — non-TEE memory node; runs `genie_memd` (one-sided RDMA memory server).

Connection is **rdma_cm** (IP-based; CM resolves GID/route/MTU — nothing hardcoded).

---

## [2026-07-23 KST] ariel — Phase 0a: bring up the genie memory node

Phase 0a validates the pure RDMA transport on **non-TEE** hosts first, before
moving the client into the SEV guest. This step: genie starts the memory server;
ariel then attaches memcached against it.

**Task for genie:**

1. Pull this repo and build the memory server (standalone, one file):
   ```sh
   cd ~/2026/memcached-1.6.42_ported/genie-server   # adjust to your clone path
   cc -O2 -o genie_memd genie_memd.c -lrdmacm -libverbs
   ```
   Prereqs: `libibverbs-dev`, `librdmacm-dev`, and an RDMA device on genie.

2. Run it (adjust size to available memory):
   ```sh
   ./genie_memd 11212 4g
   ```

3. Reply in this file with the evidence:
   - the `genie_memd: listening ...` line, and (after ariel connects) the
     `genie_memd: MR ready raddr=0x.. rkey=0x..` line
   - `ibv_devinfo -l` output (confirm a device is present)
   - confirm the process stays up (no immediate exit)

Notes:
- The server is passive: it registers one MR and hands `(raddr, rkey, size)` to
  each client connection via the rdma_cm accept `private_data`. It idles until a
  client (ariel's memcached) connects.
- The `MR ready` line is logged on the **first** client connect, so it may not
  appear until ariel attaches in the next step.

Once genie confirms it is listening, ariel attaches memcached on the non-TEE
host with `-o ext_path=<genie_ip>:11212:4g` and reports the smoke result
(`stats` counters: `extstore_engine_dead`, `extstore_write_failures`,
`extstore_read_failures`, `get_extstore`).

NEXT: genie

---

## [2026-07-23 KST] genie — Phase 0a: memory node is up, listening

Build (clean, no warnings shown):

```sh
$ cd ~/2026/memcached-1.6.42_ported/genie-server
$ cc -O2 -o genie_memd genie_memd.c -lrdmacm -libverbs
$ echo rc=$?
rc=0
```

RDMA device present:

```sh
$ ibv_devinfo -l
1 HCA found:
	ibp23s0
$ ibv_devinfo -d ibp23s0 | grep -E 'transport|state|link_layer'
	transport:			InfiniBand (0)
			state:			PORT_ACTIVE (4)
			link_layer:		InfiniBand
```

Running:

```sh
$ ./genie_memd 11212 4g
genie_memd: listening on :11212, size=4294967296
$ pgrep -a genie_memd
160562 ./genie_memd 11212 4g
```

Process stays up (still alive after >20s idle, as expected — it is passive until
a client connects). No `MR ready` line yet: per your note it is logged on the
first client connect, so it will appear once ariel attaches.

**Connect to `10.99.0.2:11212`** — that is genie's IP on `ibs3`, the netdev
backing the RDMA HCA `ibp23s0` (`/sys/class/infiniband/ibp23s0/device/net/ibs3`).
Do **not** use the 10.20.26.x mgmt address; rdma_cm will not resolve a route
over it. So: `-o ext_path=10.99.0.2:11212:4g`.

Host facts for the record: genie has 62 GB RAM, 57 GB available, so 4g MR is fine.

Go ahead and attach memcached; I will report the `MR ready raddr=/rkey=` line
from this side as soon as your client connects.

NEXT: ariel

---

## [2026-07-23 KST] ariel — memory node confirmed; client host has no RDMA HCA

Confirmed genie is up: listening on `10.99.0.2:11212` (4g), HCA `ibp23s0`
InfiniBand `PORT_ACTIVE`, passive as expected. Connect address noted
(`10.99.0.2`, not the mgmt subnet).

**Blocker for the client attach:** the host running this ariel session has **no
RDMA device**, so it cannot be the RDMA client:

```sh
$ ibv_devinfo -l
0 HCAs found:
```

The memcached RDMA client (`rdma_cm` → genie's IB fabric) must run on the
RDMA-capable ariel host (the one with an HCA on the same InfiniBand fabric as
genie, e.g. `ibp193s0`), not this box.

**Admin: please run the Phase 0a client on the RDMA host** (or grant this
session access to it). The build + attach + smoke, from a clone of this repo:

```sh
cd ~/2026/memcached-1.6.42_ported
git pull
make memcached        # if autotools tries to regen (1.17 vs 1.16 mismatch), run:
# touch configure.ac; sleep 1; touch aclocal.m4 Makefile.am; sleep 1; \
# touch configure config.h.in Makefile.in; sleep 1; \
# touch config.status config.h Makefile stamp-h1; make memcached

head -c 32 /dev/urandom > .ext.key     # crypto-on (omit EXT_CRYPTO_KEY for the off ablation)
EXT_CRYPTO_KEY=$PWD/.ext.key EXT_SLOT_SIZE=512 \
  ./memcached -p 11211 -U 0 -m 4096 \
  -o ext_path=10.99.0.2:11212:4g,ext_item_size=2,ext_threads=4
```

Then, in another shell, the smoke (value ≤ ~480 B so it fits `ext_slot_size`):

```sh
printf 'set k 0 0 5\r\nhello\r\n' | nc -q1 127.0.0.1 11211      # -> STORED
sleep 0.3
printf 'get k\r\n'               | nc -q1 127.0.0.1 11211      # -> VALUE k 0 5 / hello
printf 'stats\r\nquit\r\n'       | nc  127.0.0.1 11211 | \
  grep -E 'extstore_engine_dead|extstore_write_failures|extstore_read_failures|get_extstore|badcrc_from_extstore'
```

Success = server starts (genie logs `MR ready raddr=/rkey=`), GET returns
`hello`, and `extstore_engine_dead 0`, `write_failures 0`, `read_failures 0`,
`get_extstore ≥ 1`, `badcrc_from_extstore 0`. Paste that output here and I will
interpret it (any nonzero failure counter tells us exactly which layer broke).

If instead you want me to drive this myself, point me at the RDMA host (ssh
target) and I will run it and report.

NEXT: admin
