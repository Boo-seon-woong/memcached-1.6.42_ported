#!/usr/bin/env python3
"""Turn cpu-stage-profile.bt output into CPU-us/op and calls/op tables."""
import argparse
import re
from collections import Counter
from pathlib import Path

GROUPS = {1: ("worker", "(mc-worker)"), 2: ("io", "(memcached)"),
          3: ("client", "(memtier_benchma)")}
SYSCALLS = {
    0: "read", 1: "write", 16: "ioctl", 19: "readv", 20: "writev",
    24: "sched_yield", 46: "sendmsg", 202: "futex", 230: "clock_nanosleep",
    232: "epoll_wait", 233: "epoll_ctl",
}


def kernel_category(stack, user=""):
    if "perf_trace_run_bpf" in stack or "bpf_prog" in stack:
        return "instrumentation"
    if "sendmsg" in user or "transmit" in user:
        return "sendmsg/TCP"
    if ("pthread_" in user or "slabs_alloc" in user or "slabs_free" in user) \
            and ("do_syscall_64" in stack or "x64_sys_call" in stack):
        return "futex"
    for needle, name in (
        ("futex", "futex"),
        ("tcp_sendmsg", "sendmsg/TCP"),
        ("sendmsg", "sendmsg/TCP"),
        ("tcp_recvmsg", "read/TCP"),
        ("sock_read", "read/TCP"),
        ("epoll_wait", "epoll_wait"),
        ("mlx5_ib_advise_mr", "ioctl/dma_sync"),
        ("__x64_sys_ioctl", "ioctl/dma_sync"),
        ("sched_yield", "sched_yield"),
        ("pipe_write", "write/notify"),
        ("ksys_write", "write/notify"),
    ):
        if needle in stack:
            return name
    return "other"


def user_category(group, stack):
    if group == 2:
        if "ext_crypto_open" in stack or "libcrypto.so" in stack:
            return "AES-GCM open"
        if "_mlx5_post_send" in stack or "ibv_poll_cq" in stack or "libmlx5.so" in stack:
            return "RDMA post/poll"
        if "_storage_get_item_cb" in stack or "return_io_pending" in stack:
            return "completion callback"
        if "pthread_" in stack or "malloc" in stack or "cfree" in stack:
            return "allocator/locks"
    elif group == 1:
        if "transmit" in stack or "resp_" in stack or "sendmsg" in stack:
            return "response/TCP user"
        if "slabs_" in stack or "item_" in stack or "assoc_" in stack:
            return "item/slab/hash"
        if "drive_machine" in stack or "complete_nread" in stack or "try_read" in stack:
            return "protocol/event loop"
        if "pthread_" in stack or "malloc" in stack or "cfree" in stack:
            return "allocator/locks"
    else:
        if "parse_response" in stack or "sscanf" in stack or "evbuffer_" in stack:
            return "response parse"
        if "writev" in stack or "readv" in stack:
            return "socket user"
    return "other"


def parse_profile(text):
    samples = {g: {"user": Counter(), "kernel": Counter()} for g in GROUPS}
    pattern = re.compile(r"@samples\[(\d+), (.*?), (.*?)\]: (\d+)", re.S)
    for match in pattern.finditer(text):
        group, kernel, user, count = int(match[1]), match[2], match[3], int(match[4])
        if group not in samples:
            continue
        if kernel.strip():
            samples[group]["kernel"][kernel_category(kernel, user)] += count
        else:
            samples[group]["user"][user_category(group, user)] += count

    calls = {g: Counter() for g in GROUPS}
    for group, syscall, count in re.findall(r"@calls\[(\d+), (\d+)\]: (\d+)", text):
        calls[int(group)][SYSCALLS.get(int(syscall), f"sys_{syscall}")] += int(count)
    return samples, calls


def parse_cpu(paths):
    values = {g: [] for g in GROUPS}
    for path in paths:
        for line in Path(path).read_text().splitlines():
            fields = line.split()
            if len(fields) != 6:
                continue
            for group, (_, comm) in GROUPS.items():
                if fields[2] == comm:
                    values[group].append((float(fields[3]), float(fields[4])))
    return {
        group: (sum(v[0] for v in vals) / len(vals),
                sum(v[1] for v in vals) / len(vals))
        for group, vals in values.items() if vals
    }


def print_table(title, counts, budget):
    usable = sum(v for k, v in counts.items() if k != "instrumentation")
    print(f"\n{title}\n")
    print("| category | samples | share | CPU-us/op |")
    print("|---|---:|---:|---:|")
    for category, count in counts.most_common():
        if category == "instrumentation":
            continue
        share = count / usable if usable else 0
        print(f"| {category} | {count} | {share:.1%} | {budget * share:.3f} |")
    if counts["instrumentation"]:
        print(f"\nInstrumentation-only samples excluded: {counts['instrumentation']}.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile", nargs="?")
    parser.add_argument("--control", action="append", default=[])
    parser.add_argument("--window")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        sample = "@samples[1, , \n\tfoo+1 (/x)\n]: 2\n@calls[1, 202]: 3\n"
        samples, calls = parse_profile(sample)
        assert samples[1]["user"]["other"] == 2 and calls[1]["futex"] == 3
        print("ok")
        return
    if not args.profile or not args.control or not args.window:
        parser.error("profile, --control, and --window are required")

    text = Path(args.profile).read_text()
    samples, calls = parse_profile(text)
    cpu = parse_cpu(args.control)
    ops = int(re.search(r"\bops=(\d+)", Path(args.window).read_text())[1])

    for group, (name, _) in GROUPS.items():
        if group not in cpu:
            continue
        usr, sys = cpu[group]
        print(f"\n## {name}: baseline {usr:.3f} usr + {sys:.3f} sys = {usr + sys:.3f} CPU-us/op")
        print_table("User CPU", samples[group]["user"], usr)
        print_table("Kernel CPU", samples[group]["kernel"], sys)
        print("\n| syscall | calls | calls/op |")
        print("|---|---:|---:|")
        for syscall, count in calls[group].most_common():
            print(f"| {syscall} | {count} | {count / ops:.4f} |")


if __name__ == "__main__":
    main()
