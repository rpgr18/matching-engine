#!/usr/bin/env python3
"""Renders the README charts from benchmark JSON as static SVG, in light and dark variants.

    python3 scripts/plot_results.py [results-dir] [out-dir]

Reads <results>/m2-pro-book.json and <results>/m2-pro-depth-sweep.json and writes
throughput-{light,dark}.svg and prefetch-{light,dark}.svg. Pure standard library: the charts are
simple enough that a plotting dependency would cost more than it saves.
"""

import json
import math
import pathlib
import sys
from html import escape

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESULTS = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "results"
OUT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "docs" / "img"

FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"

THEMES = {
    "light": {
        "text": "#0b0b0b",
        "text2": "#52514e",
        "muted": "#8a8984",
        "grid": "#e4e3df",
        "axis": "#c3c2b7",
        "accent": "#2a78d6",   # this project's design
        "other": "#b9b8b1",    # alternatives it is compared against
        "series": ["#2a78d6", "#eb6834"],
        "ring": "#ffffff",
    },
    "dark": {
        "text": "#f0f6fc",
        "text2": "#c3c2b7",
        "muted": "#8f8e88",
        "grid": "#2b2b29",
        "axis": "#4a4945",
        "accent": "#3987e5",
        "other": "#5e5d58",
        "series": ["#3987e5", "#d95926"],
        "ring": "#0d1117",
    },
}

IMPL_LABEL = {
    "dense+flat": "dense ladder + flat index",
    "dense+flat+pf": "  + prefetching replay",
    "vector+flat": "sorted vector + flat index",
    "map+flat": "std::map + flat index",
    "dense+std": "dense ladder + unordered_map",
    "map+std": "std::map + unordered_map",
}
IMPL_ORDER = list(IMPL_LABEL)
OURS = {"dense+flat", "dense+flat+pf"}


def svg_open(width, height, title, desc):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" font-family="{FONT}">',
        f"<title>{escape(title)}</title><desc>{escape(desc)}</desc>",
    ]


def text(x, y, s, fill, size=12, anchor="start", weight="normal"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" font-size="{size}" '
            f'text-anchor="{anchor}" font-weight="{weight}">{escape(s)}</text>')


def hbar(x, y, w, h, fill):
    """A horizontal bar square at the baseline with a 4px rounded data end."""
    r = min(4.0, h / 2, w)
    if w <= 0:
        return ""
    return (f'<path d="M{x:.1f},{y:.1f} h{w - r:.1f} a{r},{r} 0 0 1 {r},{r} v{h - 2 * r:.1f} '
            f'a{r},{r} 0 0 1 {-r},{r} h{-(w - r):.1f} z" fill="{fill}"/>')


def nice_scale(v, max_ticks=6):
    """(axis max, tick step) covering v with at most max_ticks intervals of a round step."""
    mag = 10 ** math.floor(math.log10(v))
    for step in (mag / 10, mag / 5, mag / 4, mag / 2, mag, 2 * mag, 5 * mag):
        top = math.ceil(v / step) * step
        if top / step <= max_ticks:
            return top, step
    return 10 * mag, 2 * mag


def throughput_chart(book, theme):
    t = THEMES[theme]
    workloads = book["workloads"]
    cols, panel_w, gap = 2, 446, 36
    label_w, bar_h, bar_gap = 202, 15, 2
    impls = [i for i in IMPL_ORDER if any(r["impl"] == i for w in workloads for r in w["results"])]
    panel_h = 44 + len(impls) * (bar_h + bar_gap + 6) + 26
    rows = math.ceil(len(workloads) / cols)
    width = cols * panel_w + (cols - 1) * gap
    height = 58 + rows * panel_h + 30
    vmax, vstep = nice_scale(max(r["mreq_s"] for w in workloads for r in w["results"]), 4)
    ticks = int(round(vmax / vstep))
    plot_w = panel_w - label_w - 44

    out = svg_open(width, height, "Order book throughput by storage design",
                   "Median million requests per second for each workload and storage design; "
                   "the dense ladder with the flat index is this project's design.")
    out.append(text(0, 18, "Throughput, million requests per second (single core, median of "
                    f"{book.get('reps', 5)} replays)", t["text"], 15, weight="600"))
    m = book["machine"]
    out.append(text(0, 38, f"{m['cpu']} · {m['compiler'].split(' (')[0]} · higher is better",
                    t["text2"], 12))

    for k, w in enumerate(workloads):
        px = (k % cols) * (panel_w + gap)
        py = 58 + (k // cols) * panel_h
        res = {r["impl"]: r for r in w["results"]}
        depth = w["mean_depth"]
        depth_s = f"{depth / 1e6:.2f}M" if depth >= 1e6 else (
            f"{depth / 1e3:.0f}k" if depth >= 1e4 else f"{depth / 1e3:.1f}k")
        out.append(text(px, py + 16, w["name"], t["text"], 13, weight="600"))
        out.append(text(px + panel_w, py + 16, f"{depth_s} resting orders", t["text2"], 12,
                        anchor="end"))
        x0 = px + label_w
        top = py + 30
        # recessive grid
        for g in range(0, ticks + 1):
            gx = x0 + plot_w * g / ticks
            gv = vstep * g
            out.append(f'<line x1="{gx:.1f}" y1="{top - 4}" x2="{gx:.1f}" '
                       f'y2="{top + len(impls) * (bar_h + bar_gap + 6)}" stroke="{t["grid"]}"/>')
            out.append(text(gx, top + len(impls) * (bar_h + bar_gap + 6) + 14,
                            f"{gv:g}", t["muted"], 11, anchor="middle"))
        for j, impl in enumerate(impls):
            y = top + j * (bar_h + bar_gap + 6)
            out.append(text(x0 - 10, y + bar_h - 3, IMPL_LABEL[impl], t["text2"], 12,
                            anchor="end"))
            if impl not in res:
                continue
            v = res[impl]["mreq_s"]
            fill = t["accent"] if impl in OURS else t["other"]
            out.append(hbar(x0, y, plot_w * v / vmax, bar_h, fill))
            out.append(text(x0 + plot_w * v / vmax + 6, y + bar_h - 3, f"{v:.1f}",
                            t["text"] if impl in OURS else t["text2"], 12,
                            weight="600" if impl in OURS else "normal"))
    out.append("</svg>")
    return "\n".join(out)


def prefetch_chart(sweep, theme):
    t = THEMES[theme]
    rows = sweep["rows"]
    width, height = 720, 330
    left, right, top, bottom = 56, 150, 58, 46
    pw, ph = width - left - right, height - top - bottom
    xs = [r["orders"] for r in rows]
    lx0, lx1 = math.log10(min(xs)) - 0.05, math.log10(max(xs)) + 0.05
    vmax, vstep = nice_scale(max(max(r["plain"], r["always"]) for r in rows), 6)
    ticks = int(round(vmax / vstep))

    def X(v):
        return left + pw * (math.log10(v) - lx0) / (lx1 - lx0)

    def Y(v):
        return top + ph * (1 - v / vmax)

    out = svg_open(width, height, "Software prefetching: cost and payoff by book depth",
                   "Million requests per second against resting orders, with prefetching off and "
                   "always on; the adaptive threshold is marked.")
    out.append(text(0, 18, "Prefetching pays once the book outgrows the cache", t["text"], 15,
                    weight="600"))
    out.append(text(0, 38, "Balanced flow, single core, million requests per second by resting "
                    "orders (log scale)", t["text2"], 12))
    for g in range(0, ticks + 1):
        gv = vstep * g
        y = Y(gv)
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + pw}" y2="{y:.1f}" '
                   f'stroke="{t["grid"]}"/>')
        out.append(text(left - 8, y + 4, f"{gv:g}", t["muted"], 11, anchor="end"))
    for v, lab in [(1e4, "10k"), (3e4, "30k"), (1e5, "100k"), (3e5, "300k"), (1e6, "1M")]:
        if lx0 <= math.log10(v) <= lx1:
            out.append(text(X(v), top + ph + 18, lab, t["muted"], 11, anchor="middle"))
    # adaptive threshold
    thr = sweep.get("min_orders", 131072)
    tx = X(thr)
    out.append(f'<line x1="{tx:.1f}" y1="{top}" x2="{tx:.1f}" y2="{top + ph}" '
               f'stroke="{t["axis"]}" stroke-dasharray="4 4"/>')
    out.append(text(tx + 6, top + 12, f"replay() switches on at {thr // 1024}k orders",
                    t["text2"], 11))
    series = [("always", "prefetching on", t["series"][1]), ("plain", "prefetching off",
                                                              t["series"][0])]
    for key, label, color in series:
        pts = [(X(r["orders"]), Y(r[key])) for r in rows]
        d = " ".join(f"{'M' if i == 0 else 'L'}{x:.1f},{y:.1f}" for i, (x, y) in enumerate(pts))
        out.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="2" '
                   f'stroke-linejoin="round"/>')
        for x, y in pts:
            out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{color}" '
                       f'stroke="{t["ring"]}" stroke-width="2"/>')
        lx, ly = pts[-1]
        out.append(text(lx + 10, ly + 4, f"{label} {rows[-1][key]:.1f}", t["text"], 12))
    # legend
    lgx = left
    for key, label, color in reversed(series):
        out.append(f'<circle cx="{lgx + 5}" cy="{height - 10}" r="4" fill="{color}"/>')
        out.append(text(lgx + 14, height - 6, label, t["text2"], 12))
        lgx += 130
    out.append("</svg>")
    return "\n".join(out)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    book_path = RESULTS / "m2-pro-book.json"
    sweep_path = RESULTS / "m2-pro-depth-sweep.json"
    for theme in THEMES:
        if book_path.exists():
            (OUT / f"throughput-{theme}.svg").write_text(
                throughput_chart(json.loads(book_path.read_text()), theme))
        if sweep_path.exists():
            (OUT / f"prefetch-{theme}.svg").write_text(
                prefetch_chart(json.loads(sweep_path.read_text()), theme))
    print(f"wrote charts to {OUT}")


if __name__ == "__main__":
    main()
