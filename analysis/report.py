#!/usr/bin/env python3
"""Turn raw bypass-lab runs into latency tables, max-rate tables and plots.

    python analysis/report.py RESULTS_ROOT [--out REPORT.md] [--plots DIR]

RESULTS_ROOT holds one directory per configuration (written by
scripts/run_matrix.sh), each with <path>_<rate>.{tx.json,rx.json,samples.bin}
and a meta.json. Latency is computed from the raw per-echo samples, never from
pre-aggregated numbers:

    rtt_sched = rx - sched   (from when the packet was *due*: includes any
                              queueing behind a slow sender or receiver)
    rtt_send  = rx - send    (from when it actually left: what a naive harness
                              would report)

The first `warmup_ns` of each run (from tx.json, default 500 ms) is dropped.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

PATH_ORDER = ["recvmsg", "recvmmsg", "afxdp", "dpdk"]
# Fixed categorical order (validated reference palette, slots 1-4): a path keeps
# its colour in every chart regardless of which paths a run includes.
PATH_COLOR = {"recvmsg": "#2a78d6", "recvmmsg": "#eb6834", "afxdp": "#1baf7a", "dpdk": "#eda100"}
PCTS = [50, 99, 99.9]

SAMPLE_DTYPE = np.dtype([("seq", "<u8"), ("sched", "<i8"), ("send", "<i8"), ("rx", "<i8")])


def load_samples(path: Path) -> np.ndarray:
    raw = path.read_bytes()
    if raw[:8] != b"BLSAMP01":
        raise ValueError(f"{path}: bad magic")
    n = int(np.frombuffer(raw[8:16], "<u8")[0])
    arr = np.frombuffer(raw[16:], SAMPLE_DTYPE)
    if len(arr) != n:
        raise ValueError(f"{path}: header says {n} samples, file has {len(arr)}")
    return arr


@dataclass
class Run:
    config: str
    path: str
    rate: int
    tx: dict
    rx: dict
    rtt_sched_us: np.ndarray | None = None
    rtt_send_us: np.ndarray | None = None
    extra: dict = field(default_factory=dict)

    @property
    def lost(self) -> int:
        sent = self.tx.get("sent", 0)
        return max(0, sent - self.rx.get("packets", 0)) if self.rx else sent

    @property
    def loss_pct(self) -> float:
        sent = self.tx.get("sent", 0)
        return 100.0 * self.lost / sent if sent else float("nan")

    @property
    def checksum_ok(self) -> bool:
        return bool(self.rx) and self.rx.get("checksum") == self.tx.get("expected_checksum")


def load_runs(root: Path) -> list[Run]:
    runs = []
    for tx_path in sorted(root.glob("*/*.tx.json")):
        stem = tx_path.name[: -len(".tx.json")]
        path, _, rate = stem.rpartition("_")
        tx = json.loads(tx_path.read_text())
        rx_path = tx_path.with_name(stem + ".rx.json")
        try:
            rx = json.loads(rx_path.read_text()) if rx_path.exists() else {}
        except json.JSONDecodeError:
            rx = {}
        r = Run(tx_path.parent.name, path, int(rate), tx, rx)
        smp = tx_path.with_name(stem + ".samples.bin")
        if smp.exists():
            s = load_samples(smp)
            keep = s["sched"] >= tx.get("t0_ns", 0) + tx.get("warmup_ns", 500_000_000)
            s = s[keep]
            r.rtt_sched_us = (s["rx"] - s["sched"]) / 1e3
            r.rtt_send_us = (s["rx"] - s["send"]) / 1e3
        runs.append(r)
    return runs


def pct(a: np.ndarray | None, p: float) -> float:
    if a is None or len(a) == 0:
        return float("nan")
    return float(np.percentile(a, p))


def fmt(v: float, digits: int = 1) -> str:
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "–"
    return f"{v:,.{digits}f}"


def path_key(p: str) -> int:
    return PATH_ORDER.index(p) if p in PATH_ORDER else 99


# ---------------------------------------------------------------------------
# tables
# ---------------------------------------------------------------------------

def latency_table(runs: list[Run]) -> str:
    rows = [r for r in runs if r.rtt_sched_us is not None]
    if not rows:
        return ""
    out = ["| config | path | rate (pps) | samples | p50 µs | p99 µs | p99.9 µs | max µs "
           "| p99.9 from send µs | lost | checksum |",
           "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for r in sorted(rows, key=lambda r: (r.config, r.rate, path_key(r.path))):
        a = r.rtt_sched_us
        out.append(
            f"| {r.config} | {r.path} | {r.rate:,} | {len(a):,} | {fmt(pct(a, 50))} | {fmt(pct(a, 99))} "
            f"| {fmt(pct(a, 99.9))} | {fmt(float(a.max()) if len(a) else float('nan'))} "
            f"| {fmt(pct(r.rtt_send_us, 99.9))} | {r.lost:,} | {'ok' if r.checksum_ok else 'MISMATCH'} |")
    return "\n".join(out)


def sender_limited(r: Run) -> bool:
    """The sender delivered under 95% of the offered rate: the run measured the
    sender, so it can neither establish nor refute a receiver limit."""
    return r.tx.get("achieved_pps", 0) < 0.95 * r.rate


def maxrate_table(runs: list[Run], loss_threshold_pct: float) -> str:
    rows = [r for r in runs if r.rtt_sched_us is None]
    if not rows:
        return ""
    out = ["| config | path | offered pps | sender achieved pps | received pps | lost | loss % | note |",
           "|---|---|---:|---:|---:|---:|---:|---|"]
    best: dict[tuple[str, str], Run] = {}
    first_fail: dict[tuple[str, str], Run] = {}
    for r in sorted(rows, key=lambda r: (r.config, path_key(r.path), r.rate)):
        out.append(f"| {r.config} | {r.path} | {r.rate:,} | {fmt(r.tx.get('achieved_pps', 0), 0)} "
                   f"| {fmt(r.rx.get('rx_pps', float('nan')), 0)} | {r.lost:,} | {fmt(r.loss_pct, 3)} "
                   f"| {'sender-limited' if sender_limited(r) else ''} |")
        k = (r.config, r.path)
        if sender_limited(r):
            continue  # says nothing about the receiver either way
        if r.loss_pct <= loss_threshold_pct and k not in first_fail:
            best[k] = r
        elif r.loss_pct > loss_threshold_pct and k not in first_fail:
            first_fail[k] = r
    out += ["", f"**Max sustainable rate per core** (highest offered rate with loss ≤ "
            f"{loss_threshold_pct}% before the first failing rate; achieved = what the "
            "sender actually delivered; sender-limited runs are excluded, so if every run "
            "was sender-limited the receiver's limit is simply *not measured*):", "",
            "| config | path | max sustainable (achieved pps) | loss there | next rate | loss at next |",
            "|---|---|---:|---:|---:|---:|"]
    for k in sorted(set(best) | set(first_fail), key=lambda k: (k[0], path_key(k[1]))):
        b, f = best.get(k), first_fail.get(k)
        cap = (fmt(b.tx['achieved_pps'], 0) if f else f"≥ {fmt(b.tx['achieved_pps'], 0)} (limit not reached)") if b else "< lowest rate"
        out.append(f"| {k[0]} | {k[1]} | {cap} "
                   f"| {fmt(b.loss_pct, 3) + '%' if b else '–'} | {f'{f.rate:,}' if f else 'none failed'} "
                   f"| {fmt(f.loss_pct, 2) + '%' if f else '–'} |")
    return "\n".join(out)


def tuning_table(runs: list[Run], baseline: str) -> str:
    """Each config vs `baseline`, per (path, rate): delta of p50/p99/p99.9."""
    lat = {(r.config, r.path, r.rate): r for r in runs if r.rtt_sched_us is not None}
    configs = sorted({c for c, _, _ in lat})
    if baseline not in configs or len(configs) < 2:
        return ""
    out = [f"| path | rate | config | p50 µs | p99 µs | p99.9 µs | Δp99.9 vs `{baseline}` |",
           "|---|---:|---|---:|---:|---:|---:|"]
    keys = sorted({(p, rt) for _, p, rt in lat}, key=lambda k: (path_key(k[0]), k[1]))
    for p, rt in keys:
        base = lat.get((baseline, p, rt))
        if not base:
            continue
        b999 = pct(base.rtt_sched_us, 99.9)
        for c in [baseline] + [c for c in configs if c != baseline]:
            r = lat.get((c, p, rt))
            if not r:
                continue
            v = pct(r.rtt_sched_us, 99.9)
            d = "–" if c == baseline else f"{100 * (v - b999) / b999:+.0f}%"
            out.append(f"| {p} | {rt:,} | {c} | {fmt(pct(r.rtt_sched_us, 50))} "
                       f"| {fmt(pct(r.rtt_sched_us, 99))} | {fmt(v)} | {d} |")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# plots
# ---------------------------------------------------------------------------

def plot_percentiles(runs: list[Run], plot_dir: Path) -> list[Path]:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FixedLocator, FuncFormatter

    made = []
    groups: dict[tuple[str, int], list[Run]] = {}
    for r in runs:
        if r.rtt_sched_us is not None and len(r.rtt_sched_us):
            groups.setdefault((r.config, r.rate), []).append(r)
    # x axis: percentile on a "nines" scale, -log10(1 - p)
    ps = np.array([0, 50, 75, 90, 95, 99, 99.5, 99.9, 99.95, 99.99])
    xs = -np.log10(1 - ps / 100)
    for (config, rate), rs in sorted(groups.items()):
        fig, ax = plt.subplots(figsize=(7.5, 4.2), dpi=130)
        ymax = 0.0
        for r in sorted(rs, key=lambda r: path_key(r.path)):
            ys = np.percentile(r.rtt_sched_us, ps)
            ymax = max(ymax, float(ys[-1]))
            c = PATH_COLOR.get(r.path, "#888888")
            ax.plot(xs, ys, color=c, lw=2, marker="o", ms=4, label=r.path)
            ax.annotate(r.path, (xs[-1], ys[-1]), xytext=(6, 0), textcoords="offset points",
                        va="center", fontsize=8, color="#333333")
        ax.set_yscale("log")
        ax.xaxis.set_major_locator(FixedLocator(xs))
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: "min" if v == 0 else f"p{100 * (1 - 10 ** -v):g}"))
        ax.tick_params(axis="x", labelrotation=45, labelsize=8)
        ax.tick_params(axis="y", labelsize=8)
        ax.set_ylabel("round trip from scheduled send (µs, log)", fontsize=9, color="#333333")
        ax.set_title(f"{config}: latency by percentile at {rate:,} pps", fontsize=10, loc="left")
        ax.grid(True, which="major", color="#e6e6e6", lw=0.8)
        ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.set_xlim(xs[0] - 0.1, xs[-1] + 0.9)
        ax.legend(frameon=False, fontsize=8, loc="upper left")
        fig.tight_layout()
        out = plot_dir / f"latency_{config}_{rate}.png"
        fig.savefig(out)
        plt.close(fig)
        made.append(out)
    return made


def plot_maxrate(runs: list[Run], plot_dir: Path) -> list[Path]:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rows = [r for r in runs if r.rtt_sched_us is None]
    made = []
    for config in sorted({r.config for r in rows}):
        rs = [r for r in rows if r.config == config]
        fig, ax = plt.subplots(figsize=(7.5, 4.2), dpi=130)
        for p in sorted({r.path for r in rs}, key=path_key):
            pr = sorted([r for r in rs if r.path == p], key=lambda r: r.rate)
            x = [r.tx.get("achieved_pps", r.rate) / 1e6 for r in pr]
            y = [max(r.loss_pct, 1e-4) for r in pr]
            ax.plot(x, y, color=PATH_COLOR.get(p, "#888888"), lw=2, marker="o", ms=4, label=p)
        ax.set_yscale("log")
        ax.set_xlabel("offered load actually delivered by the sender (M packets/s)", fontsize=9)
        ax.set_ylabel("packet loss % (log; 0 drawn at 1e-4)", fontsize=9)
        ax.set_title(f"{config}: loss vs load, one receive core", fontsize=10, loc="left")
        ax.grid(True, color="#e6e6e6", lw=0.8)
        ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.legend(frameon=False, fontsize=8)
        fig.tight_layout()
        out = plot_dir / f"maxrate_{config}.png"
        fig.savefig(out)
        plt.close(fig)
        made.append(out)
    return made


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--plots", type=Path)
    ap.add_argument("--baseline", default="pinned", help="config the tuning table compares against")
    ap.add_argument("--loss-threshold", type=float, default=0.01, help="max loss %% counted as sustainable")
    ap.add_argument("--title", default="bypass-lab results")
    a = ap.parse_args(argv)

    runs = load_runs(a.root)
    if not runs:
        print(f"no runs under {a.root}", file=sys.stderr)
        return 1

    parts = [f"# {a.title}", "", f"Source: `{a.root.as_posix()}` — generated by `analysis/report.py`.", ""]
    metas = sorted(a.root.glob("*/meta.json"))
    if metas:
        m = json.loads(metas[0].read_text())
        rcv = m.get("receiver", {})
        parts += [f"Receiver: {rcv.get('cpu', '?')}, {rcv.get('nproc', '?')} vCPU, kernel "
                  f"{rcv.get('kernel', '?')}.", ""]
    t = latency_table(runs)
    if t:
        parts += ["## Round-trip latency", "",
                  "Measured on the sender's clock from the packet's *scheduled* send time to "
                  "the echo's arrival (warm-up dropped). The `from send` column is the same "
                  "samples measured from actual send time.", "", t, ""]
    t = tuning_table(runs, a.baseline)
    if t:
        parts += ["## Tuning steps, one at a time", "", t, ""]
    t = maxrate_table(runs, a.loss_threshold)
    if t:
        parts += ["## Throughput and loss", "", t, ""]

    if a.plots:
        a.plots.mkdir(parents=True, exist_ok=True)
        imgs = plot_percentiles(runs, a.plots) + plot_maxrate(runs, a.plots)
        if a.out:
            parts += ["## Plots", ""]
            for img in imgs:
                try:
                    rel = img.resolve().relative_to(a.out.resolve().parent)
                except ValueError:
                    rel = img
                parts.append(f"![{img.stem}]({rel.as_posix()})")
            parts.append("")

    text = "\n".join(parts)
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
