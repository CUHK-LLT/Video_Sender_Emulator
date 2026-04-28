#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


@dataclass(frozen=True)
class Columns:
    t_ns: str = "timestamp_ns"
    avg_rate_bps: str = "avg_rate_bps"
    target_bps: str = "target_bps"
    ingress: str = "ingress"
    egress: str = "egress"


def _require_cols(df: pd.DataFrame, cols: list[str]) -> None:
    missing = [c for c in cols if c not in df.columns]
    if missing:
        raise SystemExit(
            "CSV 缺少必要列: "
            + ", ".join(missing)
            + "\n当前列为: "
            + ", ".join(map(str, df.columns))
        )


def sliding_rate_bps(t_s: np.ndarray, bytes_counter: np.ndarray, window_s: float) -> np.ndarray:
    """
    用滑动窗口(按时间)计算速率: (bytes[t]-bytes[t-window]) * 8 / dt
    - t_s: 单调递增的秒时间戳(相对时间也可以)
    - bytes_counter: 单调递增的字节计数器
    """
    n = t_s.shape[0]
    out = np.full(n, np.nan, dtype=np.float64)
    if n == 0:
        return out

    left_times = t_s - window_s
    left_idx = np.searchsorted(t_s, left_times, side="left")

    for i in range(n):
        j = int(left_idx[i])
        if j < 0:
            j = 0
        if j >= i:
            continue
        dt = float(t_s[i] - t_s[j])
        if dt <= 0:
            continue
        db = float(bytes_counter[i] - bytes_counter[j])
        out[i] = (db * 8.0) / dt

    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="处理 echo_monitor.csv 并绘图")
    ap.add_argument(
        "--input",
        default="echo_monitor.csv",
        help="输入 CSV 路径（默认: echo_monitor.csv）",
    )
    ap.add_argument(
        "--outdir",
        default="plots",
        help="输出目录（默认: plots）",
    )
    ap.add_argument(
        "--window-ms",
        type=float,
        default=100.0,
        help="滑动窗口大小（毫秒，默认: 100）",
    )
    ap.add_argument(
        "--max-points",
        type=int,
        default=0,
        help="为加速绘图可抽样点数(0 表示不抽样)",
    )
    ap.add_argument(
        "--debug",
        action="store_true",
        help="打印关键列的统计信息（用于排查全 0/NaN/范围异常）",
    )
    ap.add_argument(
        "--show",
        action="store_true",
        help="弹出图窗显示（不加则只保存图片）",
    )
    args = ap.parse_args()

    in_path = Path(args.input)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    df = pd.read_csv(in_path)
    cols = Columns()
    _require_cols(df, [cols.t_ns, cols.avg_rate_bps, cols.target_bps, cols.ingress, cols.egress])

    df = df.sort_values(cols.t_ns, kind="stable").reset_index(drop=True)

    t_ns = df[cols.t_ns].to_numpy(dtype=np.int64)
    t_s = (t_ns - t_ns[0]).astype(np.float64) / 1e9

    avg_rate = df[cols.avg_rate_bps].to_numpy(dtype=np.float64)
    target = df[cols.target_bps].to_numpy(dtype=np.float64)
    ingress = df[cols.ingress].to_numpy(dtype=np.float64)
    egress = df[cols.egress].to_numpy(dtype=np.float64)

    if args.debug:
        def _stat(name: str, arr: np.ndarray) -> None:
            finite = np.isfinite(arr)
            n = arr.shape[0]
            nf = int(finite.sum())
            if nf == 0:
                print(f"[debug] {name}: n={n} finite=0 (all NaN/inf)")
                return
            a = arr[finite]
            print(
                f"[debug] {name}: n={n} finite={nf} "
                f"min={np.min(a):.6g} p50={np.median(a):.6g} p95={np.percentile(a,95):.6g} max={np.max(a):.6g}"
            )

        _stat(cols.avg_rate_bps, avg_rate)
        _stat(cols.target_bps, target)
        _stat(cols.ingress, ingress)
        _stat(cols.egress, egress)

    qdepth_bytes = ingress - egress
    qdepth_bytes[qdepth_bytes < 0] = 0.0

    window_s = float(args.window_ms) / 1000.0
    in_rate_bps = sliding_rate_bps(t_s, ingress, window_s)
    out_rate_bps = sliding_rate_bps(t_s, egress, window_s)

    if args.max_points and args.max_points > 0 and len(df) > args.max_points:
        idx = np.linspace(0, len(df) - 1, args.max_points).astype(int)
        t_s_p = t_s[idx]
        avg_rate_p = avg_rate[idx]
        target_p = target[idx]
        in_rate_p = in_rate_bps[idx]
        out_rate_p = out_rate_bps[idx]
        qdepth_p = qdepth_bytes[idx]
    else:
        t_s_p, avg_rate_p, target_p = t_s, avg_rate, target
        in_rate_p, out_rate_p = in_rate_bps, out_rate_bps
        qdepth_p = qdepth_bytes

    plt.rcParams.update(
        {
            "figure.dpi": 140,
            "savefig.dpi": 200,
            "axes.grid": True,
            "grid.alpha": 0.25,
        }
    )

    # One figure with two subplots (stacked):
    # - Top: avg_rate_bps, target_bps, ingress/egress rate (bps) as lines for comparison
    # - Bottom: queue depth (bytes)
    fig, (ax_top, ax_bot) = plt.subplots(
        2,
        1,
        figsize=(10.8, 6.6),
        sharex=True,
        gridspec_kw={"height_ratios": [2.1, 1.0], "hspace": 0.08},
    )

    ax_top.plot(t_s_p, avg_rate_p, linewidth=1.0, alpha=0.85, label="avg_rate_bps")
    ax_top.plot(t_s_p, target_p, linewidth=1.0, alpha=0.85, label="target_bps")
    ax_top.plot(
        t_s_p,
        in_rate_p,
        linewidth=1.0,
        alpha=0.85,
        label=f"ingress_rate_bps ({args.window_ms:g}ms window)",
    )
    ax_top.plot(
        t_s_p,
        out_rate_p,
        linewidth=1.0,
        alpha=0.85,
        label=f"egress_rate_bps ({args.window_ms:g}ms window)",
    )
    ax_top.set_ylabel("rate / bitrate (bps)")
    ax_top.set_title("Bitrate vs Ingress/Egress rate (lines)")
    ax_top.legend(loc="best", ncol=2)

    ax_bot.plot(t_s_p, qdepth_p, color="tab:gray", linewidth=1.0, alpha=0.9)
    ax_bot.set_xlabel("time (s, relative)")
    ax_bot.set_ylabel("queue depth (bytes)")
    ax_bot.set_title("Queue depth (ingress - egress)")

    fig.tight_layout()
    fig.savefig(outdir / "rate_and_queue.png")

    if args.show:
        plt.show()
    else:
        plt.close(fig)

    print(f"已输出: {(outdir / 'rate_and_queue.png').as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

