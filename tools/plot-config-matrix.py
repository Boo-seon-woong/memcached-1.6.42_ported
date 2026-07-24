#!/usr/bin/env python3
"""Render the 10-second configuration matrix as dependency-free SVG."""

import argparse
import csv
import html
from pathlib import Path

W, H = 960, 620
COLORS = {"throughput": "#2563eb", "avg": "#dc2626", "p50": "#16a34a", "p99": "#9333ea"}


def rows_for(rows, phase):
    return [r for r in rows if r["phase"] == phase and r["status"] == "ok"]


def scale(value, maximum, low, high):
    return high - value / maximum * (high - low)


def text(x, y, value, size=13, anchor="middle", weight="normal"):
    return (
        f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" '
        f'font-weight="{weight}" fill="#111827">{html.escape(str(value))}</text>'
    )


def panel(out, title, labels, series, top, bottom, y_title, threshold=None):
    left, right = 90, W - 35
    maximum = max(max(values) for values in series.values())
    if threshold is not None:
        maximum = max(maximum, threshold)
    maximum *= 1.12
    out.append(text(left, top - 18, y_title, 13, "start", "bold"))
    out.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" stroke="#374151"/>')
    out.append(f'<line x1="{left}" y1="{bottom}" x2="{right}" y2="{bottom}" stroke="#374151"/>')
    for i in range(5):
        value = maximum * i / 4
        y = scale(value, maximum, top, bottom)
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        out.append(text(left - 8, y + 4, f"{value:.1f}", 11, "end"))
    if threshold is not None:
        y = scale(threshold, maximum, top, bottom)
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" stroke="#f59e0b" stroke-dasharray="6 5"/>')
        out.append(text(right - 4, y - 5, f"{threshold:g} us objective", 11, "end"))
    step = (right - left) / max(1, len(labels) - 1)
    xs = [left + i * step for i in range(len(labels))]
    for x, label in zip(xs, labels):
        out.append(text(x, bottom + 20, label, 11))
    for name, values in series.items():
        color = COLORS[name]
        points = " ".join(f"{x:.1f},{scale(v, maximum, top, bottom):.1f}" for x, v in zip(xs, values))
        out.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="3"/>')
        for x, value in zip(xs, values):
            y = scale(value, maximum, top, bottom)
            out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{color}"/>')
        out.append(f'<rect x="{left + 150 * list(series).index(name)}" y="{top - 37}" width="13" height="4" fill="{color}"/>')
        out.append(text(left + 18 + 150 * list(series).index(name), top - 30, name, 12, "start"))
    out.append(text(W / 2, 26, title, 18, "middle", "bold"))


def line_chart(path, title, rows, x_field):
    labels = [r[x_field] for r in rows]
    throughput = [int(r["cmd_get"]) / 10 / 1_000_000 for r in rows]
    latency = {name: [float(r[f"remote_{name}_us"]) for r in rows] for name in ("avg", "p50", "p99")}
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
           '<rect width="100%" height="100%" fill="white"/>']
    panel(out, title, labels, {"throughput": throughput}, 75, 275, "Remote completed GET (M/s)")
    panel(out, "", labels, latency, 360, 565, "Remote span-v2 latency (us)", 30)
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")


def thread_chart(path, rows):
    labels = [f'{r["mt_threads"]}/{r["mc_threads"]}/{r["ext_threads"]}' for r in rows]
    throughput = [int(r["cmd_get"]) / 10 / 1_000_000 for r in rows]
    latency = {"avg": [float(r["remote_avg_us"]) for r in rows],
               "p99": [float(r["remote_p99_us"]) for r in rows]}
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
           '<rect width="100%" height="100%" fill="white"/>']
    panel(out, "Thread combinations (x = mt/mc/ext)", labels,
          {"throughput": throughput}, 75, 275, "Remote completed GET (M/s)")
    panel(out, "", labels, latency, 360, 565, "Remote span-v2 latency (us)", 30)
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")


def stock_chart(path, stock, port):
    stock_t = int(stock["cmd_get"]) / 10 / 1_000_000
    port_t = int(port["cmd_get"]) / 10 / 1_000_000
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="420" viewBox="0 0 {W} 420">',
           '<rect width="100%" height="100%" fill="white"/>',
           text(W / 2, 32, "Remote port vs stock local-memory control", 18, "middle", "bold")]
    maximum = max(stock_t, port_t) * 1.15
    for i, (label, value, color) in enumerate((("Port remote", port_t, "#2563eb"), ("Stock local", stock_t, "#6b7280"))):
        x = 230 + i * 310
        height = value / maximum * 260
        out.append(f'<rect x="{x}" y="{335-height:.1f}" width="180" height="{height:.1f}" fill="{color}"/>')
        out.append(text(x + 90, 360, label, 14))
        out.append(text(x + 90, 325 - height, f"{value:.3f} M GET/s", 14, "middle", "bold"))
    out.append(text(W / 2, 397, "Latency is intentionally not compared: port=remote span-v2, stock=memtier end-to-end.", 13))
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")


def frontier_chart(path, port, stock):
    fw, fh = 1400, 900
    by = {r["label"]: r for r in port + stock}
    value = lambda label: int(by[label]["cmd_get"]) / 10 / 1_000_000
    groups = [
        ("PIPELINE 4", [("Port QP4/D32", value("candidate-q4-d32-p4"), "#2563eb"),
                        ("Port QP6/D16", value("candidate-q6-d16-p4"), "#60a5fa"),
                        ("Stock local", value("stock-p4"), "#64748b")], "93.7% of stock"),
        ("PIPELINE 6", [("Port QP8/D16", value("candidate-q8-d16-p6"), "#2563eb"),
                        ("Stock local", value("stock-p6"), "#64748b")], "81.4% of stock"),
        ("PIPELINE 8", [("Port QP8/D16", value("candidate-q8-d16-p8"), "#1d4ed8"),
                        ("Stock local", value("stock-p8"), "#64748b")], "72.5% of stock"),
    ]
    ordered = [by[label] for label in ("candidate-q4-d32-p4", "candidate-q6-d16-p4",
                                       "candidate-q8-d16-p6", "candidate-q8-d16-p8")]
    names = ["QP4 / D32 / P4", "QP6 / D16 / P4", "QP8 / D16 / P6", "QP8 / D16 / P8"]
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{fw}" height="{fh}" viewBox="0 0 {fw} {fh}">',
           '<style>text{font-family:"DejaVu Sans","Noto Sans KR",sans-serif}</style>',
           '<rect width="100%" height="100%" fill="#f8fafc"/>',
           text(70, 54, "Remote-memory frontier vs local-memory controls", 27, "start", "bold"),
           text(70, 82, "10 s GET-only  |  mtT 8 x c16  |  mcT 8  |  Port latency includes decrypt", 14, "start")]

    x0, x1, y0, y1 = 90, 1340, 145, 445
    out += [f'<rect x="45" y="105" width="1310" height="430" rx="18" fill="white" stroke="#e2e8f0"/>',
            text(75, 137, "Completed GET throughput", 17, "start", "bold"),
            text(1325, 137, "M GET/s", 13, "end")]
    for tick in range(5):
        y = y1 - tick / 4 * (y1 - y0)
        out.append(f'<line x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}" stroke="#e2e8f0"/>')
        out.append(text(x0 - 12, y + 5, tick, 12, "end"))
    for center, (group, bars, ratio) in zip((300, 710, 1120), groups):
        bar_w, gap = 92, 20
        width = len(bars) * bar_w + (len(bars) - 1) * gap
        start = center - width / 2
        for i, (label, throughput, color) in enumerate(bars):
            x = start + i * (bar_w + gap)
            height = throughput / 4 * (y1 - y0)
            out.append(f'<rect x="{x:.1f}" y="{y1-height:.1f}" width="{bar_w}" height="{height:.1f}" rx="7" fill="{color}"/>')
            out.append(text(x + bar_w / 2, y1 - height - 10, f"{throughput:.3f}", 13, "middle", "bold"))
            out.append(text(x + bar_w / 2, y1 + 22, label, 11))
        out.append(text(center, y1 + 52, group, 13, "middle", "bold"))
        out.append(f'<rect x="{center-72}" y="{y1+63}" width="144" height="26" rx="13" fill="#eff6ff"/>')
        out.append(text(center, y1 + 81, ratio, 11, "middle", "bold"))

    lx0, lx1, ly0, ly1 = 90, 1340, 620, 830
    out += [f'<rect x="45" y="545" width="1310" height="320" rx="18" fill="white" stroke="#e2e8f0"/>',
            text(75, 582, "Port remote span-v2 latency", 17, "start", "bold"),
            text(1325, 582, "microseconds  |  line = p50 to p99  |  dot = avg", 12, "end")]
    for tick in range(0, 81, 20):
        y = ly1 - tick / 80 * (ly1 - ly0)
        out.append(f'<line x1="{lx0}" y1="{y:.1f}" x2="{lx1}" y2="{y:.1f}" stroke="#e2e8f0"/>')
        out.append(text(lx0 - 12, y + 5, tick, 12, "end"))
    objective_y = ly1 - 30 / 80 * (ly1 - ly0)
    out.append(f'<line x1="{lx0}" y1="{objective_y:.1f}" x2="{lx1}" y2="{objective_y:.1f}" stroke="#f59e0b" stroke-width="2" stroke-dasharray="8 6"/>')
    out.append(f'<rect x="{lx1-156}" y="{objective_y-27:.1f}" width="156" height="23" rx="11" fill="#fffbeb"/>')
    out.append(text(lx1 - 10, objective_y - 10, "30 us objective", 11, "end", "bold"))
    for x, name, row in zip((235, 535, 835, 1135), names, ordered):
        p50, avg, p99 = (float(row[f"remote_{field}_us"]) for field in ("p50", "avg", "p99"))
        yp50 = ly1 - p50 / 80 * (ly1 - ly0)
        yavg = ly1 - avg / 80 * (ly1 - ly0)
        yp99 = ly1 - p99 / 80 * (ly1 - ly0)
        color = "#1d4ed8" if row["label"] == "candidate-q8-d16-p8" else "#60a5fa"
        out.append(f'<line x1="{x}" y1="{yp50:.1f}" x2="{x}" y2="{yp99:.1f}" stroke="{color}" stroke-width="14" stroke-linecap="round" opacity="0.45"/>')
        out.append(f'<circle cx="{x}" cy="{yp50:.1f}" r="6" fill="#16a34a"/>')
        out.append(f'<circle cx="{x}" cy="{yp99:.1f}" r="6" fill="#7c3aed"/>')
        out.append(f'<circle cx="{x}" cy="{yavg:.1f}" r="9" fill="#dc2626" stroke="white" stroke-width="3"/>')
        out.append(text(x, yp99 - 12, f"p99 {p99:.1f}", 11, "middle", "bold"))
        out.append(text(x, ly1 + 25, name, 12, "middle", "bold"))
        out.append(text(x, ly1 + 45, f"avg {avg:.3f} us", 11))
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")


def target_chart(path, port, stock):
    tw, th = 1200, 650
    port_t = int(port["cmd_get"]) / 10 / 1_000_000
    stock_t = int(stock["cmd_get"]) / 10 / 1_000_000
    x0, x1 = 245, 1100
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{tw}" height="{th}" viewBox="0 0 {tw} {th}">',
           '<style>text{font-family:"DejaVu Sans","Noto Sans KR",sans-serif}</style>',
           '<rect width="100%" height="100%" fill="#f8fafc"/>',
           text(60, 58, "The 10M target is outside the same-shape local ceiling", 27, "start", "bold"),
           text(60, 88, "Pipeline 8  |  mtT 8 x c16  |  mcT 8  |  10-second completed GET count", 14, "start"),
           '<rect x="45" y="120" width="1110" height="310" rx="18" fill="white" stroke="#e2e8f0"/>']
    for tick in range(0, 11, 2):
        x = x0 + tick / 10 * (x1 - x0)
        out.append(f'<line x1="{x:.1f}" y1="160" x2="{x:.1f}" y2="370" stroke="#e2e8f0"/>')
        out.append(text(x, 397, f"{tick}M", 12))
    out.append(f'<line x1="{x1}" y1="145" x2="{x1}" y2="375" stroke="#dc2626" stroke-width="4"/>')
    out.append(f'<rect x="{x1-126}" y="134" width="126" height="28" rx="14" fill="#fee2e2"/>')
    out.append(text(x1 - 12, 154, "10M target", 12, "end", "bold"))
    for y, label, value, color in (
        (215, "Port QP8 / depth16 / pipeline8", port_t, "#2563eb"),
        (315, "Stock local / pipeline8", stock_t, "#64748b"),
    ):
        width = value / 10 * (x1 - x0)
        out.append(text(x0 - 18, y + 6, label, 13, "end", "bold"))
        out.append(f'<rect x="{x0}" y="{y-24}" width="{x1-x0}" height="48" rx="10" fill="#f1f5f9"/>')
        out.append(f'<rect x="{x0}" y="{y-24}" width="{width:.1f}" height="48" rx="10" fill="{color}"/>')
        out.append(text(x0 + width + 12, y + 6, f"{value:.3f} M/s  ({value*10:.1f}% of target)", 13, "start", "bold"))
    cards = [
        (55, "PORT / STOCK", f"{port_t/stock_t*100:.1f}%", "remote path retains most local throughput"),
        (405, "STOCK TO 10M", f"{10/stock_t:.2f}x", "required after removing RDMA and crypto"),
        (755, "PORT TO 10M", f"{10/port_t:.2f}x", "required at the <30 us optimum"),
    ]
    for x, heading, metric, note in cards:
        out.append(f'<rect x="{x}" y="465" width="310" height="125" rx="16" fill="white" stroke="#e2e8f0"/>')
        out.append(text(x + 22, 495, heading, 11, "start", "bold"))
        out.append(text(x + 22, 540, metric, 29, "start", "bold"))
        out.append(text(x + 22, 568, note, 11, "start"))
    out.append(text(600, 625, "Scope: same pipeline/thread shape; stock pipeline 8 was still scaling and is not an absolute hardware ceiling.", 12))
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv")
    parser.add_argument("out")
    args = parser.parse_args()
    with open(args.csv, newline="") as source:
        rows = list(csv.DictReader(source))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    frontier = rows_for(rows, "frontier")
    if frontier:
        stock = rows_for(rows, "stock")
        assert len(frontier) == 4 and len(stock) == 3, "frontier matrix is incomplete"
        frontier_chart(out / "frontier-seven-points.svg", frontier, stock)
        target_chart(out / "frontier-vs-10m.svg",
                     next(r for r in frontier if r["label"] == "candidate-q8-d16-p8"),
                     next(r for r in stock if r["pipeline"] == "8"))
        print(*(str(p) for p in sorted(out.glob("*.svg"))), sep="\n")
        return
    phases = {name: rows_for(rows, name) for name in ("qp", "pipeline", "threads", "depth", "stock")}
    assert all(phases.values()), "matrix is incomplete"
    line_chart(out / "qp-sweep.svg", "QP/ext-thread sweep", phases["qp"], "qp")
    line_chart(out / "pipeline-sweep.svg", "Pipeline sweep", phases["pipeline"], "pipeline")
    thread_chart(out / "thread-combinations.svg", phases["threads"])
    line_chart(out / "depth-sweep.svg", "QP depth sweep", phases["depth"], "depth")
    port = next(r for r in phases["pipeline"] if r["pipeline"] == "4")
    stock_chart(out / "stock-throughput.svg", phases["stock"][0], port)
    print(*(str(p) for p in sorted(out.glob("*.svg"))), sep="\n")


if __name__ == "__main__":
    main()
