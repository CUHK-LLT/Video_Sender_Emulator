#!/usr/bin/env python3
"""Analyze L2T1 send/recv/playback CSV logs and print metrics to console."""

from __future__ import annotations

import argparse
import bisect
import csv
import math
import statistics
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass
class SendFrameRow:
    frame_id: int
    planned_frame_packets: int
    sent_frame_packets: int


@dataclass
class PlaybackRow:
    deadline_us: int
    temporal_unit_id: int
    chosen_sid: int
    stall_us: int
    decodable_bytes: int
    total_bytes_est: int
    quality_est: float


@dataclass
class RecvAgg:
    tu_key: int
    sid: int
    first_ts_us: int
    last_ts_us: int
    eof_ts_us: Optional[int]
    unique_packets: int
    expected_packets: int
    bytes_total: int
    bytes_before_deadline: int


def parse_ratios(raw: str) -> Tuple[float, float]:
    parts = raw.replace(":", ",").replace("/", ",").split(",")
    if len(parts) != 2:
        raise ValueError(f"--spatial-ratios must be two values, got: {raw!r}")
    r0 = float(parts[0])
    r1 = float(parts[1])
    if r0 < 0 or r1 < 0 or (r0 == 0 and r1 == 0):
        raise ValueError(f"--spatial-ratios invalid, got: {raw!r}")
    return r0, r1


def split_packets_l2t1(total_packets: int, r0: float, r1: float) -> Tuple[int, int]:
    if total_packets <= 0:
        return 0, 0
    ratio_sum = (r0 + r1) if (r0 + r1) > 0 else 1.0
    s0 = int(round(total_packets * (r0 / ratio_sum)))
    if s0 == 0:
        s0 = 1
    if s0 > total_packets:
        s0 = total_packets
    s1 = total_packets - s0
    return s0, s1


def quantile(sorted_vals: List[float], q: float) -> Optional[float]:
    if not sorted_vals:
        return None
    if q <= 0:
        return float(sorted_vals[0])
    if q >= 1:
        return float(sorted_vals[-1])
    pos = (len(sorted_vals) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_vals[lo])
    w = pos - lo
    return float(sorted_vals[lo] * (1 - w) + sorted_vals[hi] * w)


def fmt(x: Optional[float], nd: int = 4) -> str:
    if x is None:
        return "NA"
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return "NA"
    return f"{x:.{nd}f}"


def load_send_csv(path: str) -> Dict[int, SendFrameRow]:
    by_frame: Dict[int, SendFrameRow] = {}
    with open(path, "r", newline="", encoding="utf-8") as fh:
        rd = csv.DictReader(fh)
        required = {"frame_id", "planned_frame_packets", "sent_frame_packets"}
        missing = required - set(rd.fieldnames or [])
        if missing:
            raise RuntimeError(f"send csv missing columns {sorted(missing)}")
        for row in rd:
            frame_id = int(row["frame_id"])
            planned = int(row["planned_frame_packets"])
            sent = int(row["sent_frame_packets"])
            prev = by_frame.get(frame_id)
            if prev is None:
                by_frame[frame_id] = SendFrameRow(frame_id, planned, sent)
            else:
                by_frame[frame_id] = SendFrameRow(
                    frame_id=frame_id,
                    planned_frame_packets=prev.planned_frame_packets + planned,
                    sent_frame_packets=prev.sent_frame_packets + sent,
                )
    return by_frame


def load_playback_csv(path: str) -> List[PlaybackRow]:
    rows: List[PlaybackRow] = []
    with open(path, "r", newline="", encoding="utf-8") as fh:
        rd = csv.DictReader(fh)
        required = {
            "deadline_us",
            "temporal_unit_id",
            "chosen_sid",
            "stall_us",
            "decodable_bytes",
            "total_bytes",
            "quality",
        }
        missing = required - set(rd.fieldnames or [])
        if missing:
            raise RuntimeError(f"playback csv missing columns {sorted(missing)}")
        for row in rd:
            rows.append(
                PlaybackRow(
                    deadline_us=int(row["deadline_us"]),
                    temporal_unit_id=int(row["temporal_unit_id"]),
                    chosen_sid=int(row["chosen_sid"]),
                    stall_us=int(row["stall_us"]),
                    decodable_bytes=int(row["decodable_bytes"]),
                    total_bytes_est=int(row["total_bytes"]),
                    quality_est=float(row["quality"]),
                )
            )
    rows.sort(key=lambda r: r.temporal_unit_id)
    return rows


def _mean(values: List[float]) -> Optional[float]:
    return statistics.fmean(values) if values else None


def _std(values: List[float]) -> Optional[float]:
    if len(values) < 2:
        return 0.0 if values else None
    return statistics.pstdev(values)


def _stats_block(name: str, values: List[float], nd: int = 4) -> None:
    if not values:
        print(f"{name}: NA")
        return
    s = sorted(values)
    print(
        f"{name}: mean={fmt(_mean(s), nd)} p50={fmt(quantile(s, 0.50), nd)} "
        f"p90={fmt(quantile(s, 0.90), nd)} p99={fmt(quantile(s, 0.99), nd)} "
        f"min={fmt(s[0], nd)} max={fmt(s[-1], nd)}"
    )


def _run_lengths(values: Iterable[int], target: int) -> List[int]:
    runs: List[int] = []
    cur = 0
    for v in values:
        if v == target:
            cur += 1
        else:
            if cur > 0:
                runs.append(cur)
                cur = 0
    if cur > 0:
        runs.append(cur)
    return runs


def load_recv_and_aggregate(
    path: str,
    playback_deadline: Dict[int, int],
    join_key: str,
    dedup_key: str,
    window_ms: int,
    seq_end_tu: Optional[List[Tuple[int, int]]],
) -> Tuple[Dict[Tuple[int, int], RecvAgg], Dict[str, float], List[Tuple[int, int, int, int]]]:
    """
    Returns:
      - per (tu_key, sid) aggregation
      - recv-level packet stats
      - window stats list: (window_idx, bytes, packets, out_of_order_packets)
    """
    with open(path, "r", newline="", encoding="utf-8") as fh:
        rd = csv.DictReader(fh)
        required = {
            "timestamp_us",
            "frame_id",
            "packet_id",
            "frame_packet_count",
            "send_seq",
            "pkt_len",
            "av1_eof",
            "av1_spatial_id",
            "av1_frame_number",
        }
        missing = required - set(rd.fieldnames or [])
        if missing:
            raise RuntimeError(f"recv csv missing columns {sorted(missing)}")

        seen = set()
        aggs: Dict[Tuple[int, int], RecvAgg] = {}
        dup_packets = 0
        total_packets = 0
        unique_packets = 0
        out_of_order_packets = 0
        reorder_depths: List[int] = []
        max_seq_seen: Optional[int] = None
        ts0: Optional[int] = None
        windows: Dict[int, List[int]] = {}
        end_list = [x[0] for x in seq_end_tu] if seq_end_tu else []

        for row in rd:
            total_packets += 1
            ts_us = int(row["timestamp_us"])
            frame_id = int(row["frame_id"])
            frame_num = int(row["av1_frame_number"])
            packet_id = int(row["packet_id"])
            frame_packet_count = int(row["frame_packet_count"])
            send_seq = int(row["send_seq"])
            pkt_len = int(row["pkt_len"])
            sid = int(row["av1_spatial_id"])
            av1_eof = int(row["av1_eof"])

            if join_key == "frame_id":
                tu_key = frame_id
            elif join_key == "av1_frame_number":
                tu_key = frame_num
            elif join_key == "send_seq_tu":
                if not seq_end_tu:
                    raise RuntimeError("join_key=send_seq_tu requires send csv mapping")
                idx = bisect.bisect_left(end_list, send_seq)
                if idx >= len(seq_end_tu):
                    # recv packet outside sender range in send csv; skip
                    continue
                tu_key = seq_end_tu[idx][1]
            else:
                raise RuntimeError(f"Unsupported join key: {join_key}")

            if dedup_key == "send_seq":
                dkey = ("seq", send_seq)
            else:
                dkey = ("fp", frame_id, packet_id)

            if dkey in seen:
                dup_packets += 1
                continue
            seen.add(dkey)
            unique_packets += 1

            if max_seq_seen is None:
                max_seq_seen = send_seq
            else:
                if send_seq < max_seq_seen:
                    out_of_order_packets += 1
                    reorder_depths.append(max_seq_seen - send_seq)
                elif send_seq > max_seq_seen:
                    max_seq_seen = send_seq

            if ts0 is None:
                ts0 = ts_us
            widx = (ts_us - ts0) // (window_ms * 1000)
            win = windows.setdefault(int(widx), [0, 0, 0])
            win[0] += pkt_len
            win[1] += 1
            if max_seq_seen is not None and send_seq < max_seq_seen:
                win[2] += 1

            key = (tu_key, sid)
            deadline_us = playback_deadline.get(tu_key)
            agg = aggs.get(key)
            if agg is None:
                agg = RecvAgg(
                    tu_key=tu_key,
                    sid=sid,
                    first_ts_us=ts_us,
                    last_ts_us=ts_us,
                    eof_ts_us=(ts_us if av1_eof == 1 else None),
                    unique_packets=1,
                    expected_packets=frame_packet_count,
                    bytes_total=pkt_len,
                    bytes_before_deadline=(pkt_len if deadline_us is not None and ts_us <= deadline_us else 0),
                )
                aggs[key] = agg
            else:
                agg.first_ts_us = min(agg.first_ts_us, ts_us)
                agg.last_ts_us = max(agg.last_ts_us, ts_us)
                if av1_eof == 1:
                    agg.eof_ts_us = ts_us if agg.eof_ts_us is None else min(agg.eof_ts_us, ts_us)
                agg.unique_packets += 1
                agg.expected_packets = max(agg.expected_packets, frame_packet_count)
                agg.bytes_total += pkt_len
                if deadline_us is not None and ts_us <= deadline_us:
                    agg.bytes_before_deadline += pkt_len

    reorder_depths_sorted = sorted(float(x) for x in reorder_depths)
    win_rows = sorted((k, v[0], v[1], v[2]) for k, v in windows.items())
    packet_stats = {
        "total_packets": float(total_packets),
        "unique_packets": float(unique_packets),
        "duplicate_packets": float(dup_packets),
        "duplicate_rate": (dup_packets / total_packets) if total_packets > 0 else float("nan"),
        "out_of_order_packets": float(out_of_order_packets),
        "out_of_order_rate": (out_of_order_packets / unique_packets) if unique_packets > 0 else float("nan"),
        "reorder_depth_p50": quantile(reorder_depths_sorted, 0.50) or 0.0,
        "reorder_depth_p90": quantile(reorder_depths_sorted, 0.90) or 0.0,
        "reorder_depth_p99": quantile(reorder_depths_sorted, 0.99) or 0.0,
    }
    return aggs, packet_stats, win_rows


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze L2T1 send/recv/playback logs. "
            "Includes playability, layer stability, true-denominator quality, "
            "recv completion/deadline margin, out-of-order and goodput jitter."
        )
    )
    parser.add_argument("--send-csv", required=True, help="Path to send_*.csv")
    parser.add_argument("--recv-csv", required=True, help="Path to recv_*.csv")
    parser.add_argument("--playback-csv", required=True, help="Path to playback_*.csv")
    parser.add_argument("--spatial-ratios", default="1,1", help="S0,S1 ratio, e.g. 1,1 or 1,3")
    parser.add_argument("--pkt-size", type=int, default=1200, help="Sender pkt-size (bytes)")
    parser.add_argument("--fps", type=float, default=30.0, help="Playout fps")
    parser.add_argument("--window-ms", type=int, default=1000, help="Window size for goodput stats")
    parser.add_argument(
        "--join-key",
        choices=["send_seq_tu", "frame_id", "av1_frame_number"],
        default="send_seq_tu",
        help="How recv rows join to playback/send keys",
    )
    parser.add_argument(
        "--dedup-key",
        choices=["send_seq", "frame_packet"],
        default="send_seq",
        help="Packet dedup key for recv analysis",
    )
    parser.add_argument(
        "--denom",
        choices=["sent", "planned"],
        default="sent",
        help="Use sent_frame_packets or planned_frame_packets for true denominator",
    )
    parser.add_argument(
        "--completion-ts",
        choices=["eof_or_last", "last"],
        default="eof_or_last",
        help="Use eof timestamp when available or always use last packet timestamp",
    )
    parser.add_argument(
        "--gbd-denom",
        choices=["sender", "recv"],
        default="sender",
        help="Denominator for goodput-before-deadline ratio",
    )
    parser.add_argument("--top-n", type=int, default=8, help="Print top-N worst cases by margin")
    args = parser.parse_args()

    r0, r1 = parse_ratios(args.spatial_ratios)
    send_map = load_send_csv(args.send_csv)
    playback_rows = load_playback_csv(args.playback_csv)
    if not playback_rows:
        raise SystemExit("playback csv is empty")

    playback_by_tu = {r.temporal_unit_id: r for r in playback_rows}
    playback_deadline = {r.temporal_unit_id: r.deadline_us for r in playback_rows}
    send_rows_sorted = sorted(send_map.values(), key=lambda x: x.frame_id)
    seq_end_tu: List[Tuple[int, int]] = []
    seq_cursor = 0
    for row in send_rows_sorted:
        pkts = row.sent_frame_packets if args.denom == "sent" else row.planned_frame_packets
        if pkts <= 0:
            continue
        seq_cursor += pkts
        seq_end_tu.append((seq_cursor - 1, row.frame_id))

    # 1) Playability / stall
    n = len(playback_rows)
    stall_flags = [(r.chosen_sid == -1) or (r.stall_us > 0) for r in playback_rows]
    stall_count = sum(1 for x in stall_flags if x)
    playable_rate = (n - stall_count) / n

    # 2) Layer occupancy / switching
    chosen = [r.chosen_sid for r in playback_rows]
    s1_count = sum(1 for c in chosen if c == 1)
    s0_count = sum(1 for c in chosen if c == 0)
    non_stall = sum(1 for c in chosen if c >= 0)
    switches = sum(1 for i in range(1, n) if chosen[i] != chosen[i - 1])
    stall_runs = sorted(float(x) for x in _run_lengths(chosen, -1))

    # 6) True denominator quality from send + playback
    q_true: List[float] = []
    q_est: List[float] = []
    missing_send = 0
    for p in playback_rows:
        if p.chosen_sid < 0:
            continue
        srow = send_map.get(p.temporal_unit_id)
        if srow is None:
            missing_send += 1
            continue
        total_packets = srow.sent_frame_packets if args.denom == "sent" else srow.planned_frame_packets
        s0_pkts, s1_pkts = split_packets_l2t1(total_packets, r0, r1)
        denom_bytes = (s1_pkts if p.chosen_sid == 1 else s0_pkts) * args.pkt_size
        if denom_bytes <= 0:
            continue
        q_true.append(p.decodable_bytes / float(denom_bytes))
        q_est.append(p.quality_est)

    # recv aggregation
    recv_aggs, pkt_stats, window_rows = load_recv_and_aggregate(
        path=args.recv_csv,
        playback_deadline=playback_deadline,
        join_key=args.join_key,
        dedup_key=args.dedup_key,
        window_ms=args.window_ms,
        seq_end_tu=seq_end_tu,
    )

    margins_all: List[float] = []
    comp_ratio_all: List[float] = []
    gbd_all: List[float] = []
    per_sid = {0: {"margins": [], "comp": [], "gbd": []}, 1: {"margins": [], "comp": [], "gbd": []}}
    worst_cases: List[Tuple[float, int, int, float, float]] = []

    for (tu, sid), agg in recv_aggs.items():
        pb = playback_by_tu.get(tu)
        if pb is None:
            continue
        completion_ts = (
            agg.last_ts_us
            if args.completion_ts == "last"
            else (agg.eof_ts_us if agg.eof_ts_us is not None else agg.last_ts_us)
        )
        margin = completion_ts - pb.deadline_us
        exp_pkts = agg.expected_packets if agg.expected_packets > 0 else agg.unique_packets
        comp_ratio = agg.unique_packets / float(exp_pkts) if exp_pkts > 0 else 0.0

        if args.gbd_denom == "recv":
            gbd_denom = agg.bytes_total
        else:
            srow = send_map.get(tu)
            if srow is None:
                gbd_denom = agg.bytes_total
            else:
                total_packets = srow.sent_frame_packets if args.denom == "sent" else srow.planned_frame_packets
                s0_pkts, s1_pkts = split_packets_l2t1(total_packets, r0, r1)
                gbd_denom = (s0_pkts if sid == 0 else s1_pkts) * args.pkt_size
        gbd = agg.bytes_before_deadline / float(gbd_denom) if gbd_denom > 0 else 0.0
        gbd = min(gbd, 1.0)

        margins_all.append(float(margin))
        comp_ratio_all.append(float(comp_ratio))
        gbd_all.append(float(gbd))
        if sid in per_sid:
            per_sid[sid]["margins"].append(float(margin))
            per_sid[sid]["comp"].append(float(comp_ratio))
            per_sid[sid]["gbd"].append(float(gbd))
        worst_cases.append((float(margin), tu, sid, float(comp_ratio), float(gbd)))

    # S1/S0 delta completion time
    s1_minus_s0: List[float] = []
    for tu in playback_by_tu:
        a0 = recv_aggs.get((tu, 0))
        a1 = recv_aggs.get((tu, 1))
        if a0 is None or a1 is None:
            continue
        c0 = a0.eof_ts_us if (args.completion_ts == "eof_or_last" and a0.eof_ts_us is not None) else a0.last_ts_us
        c1 = a1.eof_ts_us if (args.completion_ts == "eof_or_last" and a1.eof_ts_us is not None) else a1.last_ts_us
        s1_minus_s0.append(float(c1 - c0))

    # S1 aggressive / could-up-not-up
    s1_aggressive = 0
    s1_aggressive_base = 0
    could_up_not_up = 0
    could_up_not_up_base = 0
    for p in playback_rows:
        a1 = recv_aggs.get((p.temporal_unit_id, 1))
        if a1 is None:
            continue
        c1 = a1.eof_ts_us if (args.completion_ts == "eof_or_last" and a1.eof_ts_us is not None) else a1.last_ts_us
        m1 = c1 - p.deadline_us
        exp_pkts = a1.expected_packets if a1.expected_packets > 0 else a1.unique_packets
        r1_comp = a1.unique_packets / float(exp_pkts) if exp_pkts > 0 else 0.0
        s1_ready = (m1 <= 0) and (r1_comp >= 1.0)

        if p.chosen_sid == 1:
            s1_aggressive_base += 1
            if (m1 > 0) or (r1_comp < 1.0):
                s1_aggressive += 1
        if p.chosen_sid == 0:
            could_up_not_up_base += 1
            if s1_ready:
                could_up_not_up += 1

    # window stats
    window_goodput_bps: List[float] = []
    window_ooo_rate: List[float] = []
    for _, bytes_sum, pkts_sum, ooo_sum in window_rows:
        if args.window_ms > 0:
            bps = bytes_sum * 8.0 / (args.window_ms / 1000.0)
            window_goodput_bps.append(bps)
        if pkts_sum > 0:
            window_ooo_rate.append(ooo_sum / float(pkts_sum))
    gp_mean = _mean(window_goodput_bps)
    gp_std = _std(window_goodput_bps)
    gp_cv = (gp_std / gp_mean) if (gp_std is not None and gp_mean not in (None, 0.0)) else None

    # optional utility
    util: List[float] = []
    for p in playback_rows:
        if (p.chosen_sid == -1) or (p.stall_us > 0):
            util.append(-1.0)
            continue
        q = p.quality_est
        srow = send_map.get(p.temporal_unit_id)
        if srow is not None:
            total_packets = srow.sent_frame_packets if args.denom == "sent" else srow.planned_frame_packets
            s0_pkts, s1_pkts = split_packets_l2t1(total_packets, r0, r1)
            denom_bytes = (s1_pkts if p.chosen_sid == 1 else s0_pkts) * args.pkt_size
            if denom_bytes > 0:
                q = p.decodable_bytes / float(denom_bytes)
        util.append(q)

    # print report
    duration_s = n / args.fps if args.fps > 0 else float("nan")
    print("=== Inputs ===")
    print(f"send_csv            = {args.send_csv}")
    print(f"recv_csv            = {args.recv_csv}")
    print(f"playback_csv        = {args.playback_csv}")
    print(f"spatial_ratios      = S0:{r0} S1:{r1}")
    print(f"pkt_size            = {args.pkt_size}")
    print(f"join_key            = {args.join_key}")
    print(f"dedup_key           = {args.dedup_key}")
    print(f"denom               = {args.denom}")
    print(f"window_ms           = {args.window_ms}")
    print()

    print("=== 1) Playability / Stall ===")
    print(f"frames_total        = {n}")
    print(f"duration_s_approx   = {fmt(duration_s, 3)}")
    print(f"stall_frames        = {stall_count} ({fmt(stall_count / n * 100.0, 2)}%)")
    print(f"playable_rate       = {fmt(playable_rate, 4)}")
    if stall_runs:
        print(
            f"stall_run(frames)   = p50 {fmt(quantile(stall_runs, 0.50),2)} / "
            f"p90 {fmt(quantile(stall_runs, 0.90),2)} / "
            f"p99 {fmt(quantile(stall_runs, 0.99),2)} / "
            f"max {fmt(stall_runs[-1],2)}"
        )
    else:
        print("stall_run(frames)   = no stall runs")
    print()

    print("=== 2) Layer Occupancy / Switching ===")
    print(f"chosen_sid_1        = {s1_count} ({fmt(s1_count / n * 100.0, 2)}% of all)")
    print(f"chosen_sid_0        = {s0_count} ({fmt(s0_count / n * 100.0, 2)}% of all)")
    print(f"chosen_sid_-1       = {n - s1_count - s0_count} ({fmt((n - s1_count - s0_count) / n * 100.0, 2)}% of all)")
    s1_nonstall = (s1_count / non_stall) if non_stall > 0 else float("nan")
    print(f"s1_rate_non_stall   = {fmt(s1_nonstall * 100.0, 2)}%")
    print(f"switches_total      = {switches}")
    print(f"switch_rate_per100f = {fmt(switches / n * 100.0, 3)}")
    print()

    print("=== 6) Quality With Scheme-B Denominator ===")
    print(f"quality_samples     = {len(q_true)}")
    print(f"missing_send_rows   = {missing_send}")
    _stats_block("quality_est(csv)", q_est)
    _stats_block("quality_true(B)", q_true)
    print()

    print("=== Receiver Packet-Level Stats ===")
    print(f"total_packets       = {int(pkt_stats['total_packets'])}")
    print(f"unique_packets      = {int(pkt_stats['unique_packets'])}")
    print(f"duplicate_packets   = {int(pkt_stats['duplicate_packets'])} ({fmt(pkt_stats['duplicate_rate'] * 100.0, 3)}%)")
    print(
        f"out_of_order        = {int(pkt_stats['out_of_order_packets'])} "
        f"({fmt(pkt_stats['out_of_order_rate'] * 100.0, 3)}%)"
    )
    print(
        "reorder_depth(seq)  = "
        f"p50 {fmt(pkt_stats['reorder_depth_p50'], 2)} / "
        f"p90 {fmt(pkt_stats['reorder_depth_p90'], 2)} / "
        f"p99 {fmt(pkt_stats['reorder_depth_p99'], 2)}"
    )
    print()

    print("=== Receiver Completion / Deadline (all layers) ===")
    if margins_all:
        miss_rate = sum(1 for m in margins_all if m > 0) / len(margins_all)
    else:
        miss_rate = float("nan")
    print(f"joined_layer_samples= {len(margins_all)}")
    print(f"deadline_miss_rate  = {fmt(miss_rate * 100.0, 3)}%")
    _stats_block("margin_us", margins_all, nd=2)
    _stats_block("completion_ratio", comp_ratio_all)
    _stats_block("goodput_before_deadline_ratio", gbd_all)
    print()

    print("=== Receiver Completion / Deadline (per sid) ===")
    for sid in (0, 1):
        vals = per_sid[sid]
        print(f"[sid={sid}] samples={len(vals['margins'])}")
        _stats_block("  margin_us", vals["margins"], nd=2)
        _stats_block("  completion_ratio", vals["comp"])
        _stats_block("  goodput_before_deadline_ratio", vals["gbd"])
    print()

    print("=== Cross-Layer Explainability ===")
    _stats_block("s1_minus_s0_completion_us", s1_minus_s0, nd=2)
    aggr_rate = (s1_aggressive / s1_aggressive_base) if s1_aggressive_base > 0 else float("nan")
    miss_up_rate = (could_up_not_up / could_up_not_up_base) if could_up_not_up_base > 0 else float("nan")
    print(f"s1_aggressive_events= {s1_aggressive}/{s1_aggressive_base} ({fmt(aggr_rate * 100.0, 3)}%)")
    print(f"could_up_not_up     = {could_up_not_up}/{could_up_not_up_base} ({fmt(miss_up_rate * 100.0, 3)}%)")
    print()

    print("=== Windowed Dynamic Stats ===")
    _stats_block("window_goodput_bps", window_goodput_bps, nd=2)
    print(f"window_goodput_cv   = {fmt(gp_cv, 4)}")
    _stats_block("window_ooo_rate", window_ooo_rate)
    print()

    util_sorted = sorted(util)
    _stats_block("utility_simple(q_or_-1)", util_sorted)
    print()

    print(f"=== Top-{args.top_n} Worst Cases By Margin ===")
    worst_cases.sort(reverse=True, key=lambda x: x[0])
    print("margin_us,tu_key,sid,completion_ratio,gbd_ratio")
    for row in worst_cases[: max(0, args.top_n)]:
        print(f"{int(row[0])},{row[1]},{row[2]},{fmt(row[3],4)},{fmt(row[4],4)}")


if __name__ == "__main__":
    main()
