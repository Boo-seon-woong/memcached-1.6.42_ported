# memcached-1.6.42_ported — ariel/genie deployment channel

Fresh coordination channel for the extstore **RDMA remote-memory** port. This
repo IS the code; genie builds directly from it. Append-only log.

Convention:
- Each entry ends with exactly one `NEXT: genie` / `NEXT: ariel` / `NEXT: none`.
- Commit prefixes: `[ariel]` / `[genie]`. Pull before writing.
- Report commands as fenced code blocks with their key output (not just prose).

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
