"""Tests for analysis/report.py on synthetic runs (pytest)."""
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import report  # noqa: E402


def write_run(d: Path, path: str, rate: int, lat_ns: np.ndarray | None, sent: int, received: int,
              warm: int = 0, same_checksum: bool = True):
    d.mkdir(parents=True, exist_ok=True)
    t0 = 1_000_000_000
    tx = {"sent": sent, "achieved_pps": float(rate), "expected_checksum": "ab", "t0_ns": t0,
          "warmup_ns": warm}
    (d / f"{path}_{rate}.tx.json").write_text(json.dumps(tx))
    rx = {"packets": received, "checksum": "ab" if same_checksum else "cd", "rx_pps": float(rate)}
    (d / f"{path}_{rate}.rx.json").write_text(json.dumps(rx))
    if lat_ns is not None:
        n = len(lat_ns)
        sched = t0 + np.arange(n, dtype=np.int64) * 1000
        arr = np.zeros(n, report.SAMPLE_DTYPE)
        arr["seq"] = np.arange(n)
        arr["sched"] = sched
        arr["send"] = sched + 100
        arr["rx"] = sched + lat_ns
        (d / f"{path}_{rate}.samples.bin").write_bytes(b"BLSAMP01" + struct.pack("<Q", n) + arr.tobytes())


def test_percentiles_and_warmup(tmp_path):
    lat = np.full(10_000, 20_000, dtype=np.int64)
    lat[:100] = 10_000_000  # the first 100 samples are slow -- but inside warm-up
    write_run(tmp_path / "pinned", "dpdk", 10000, lat, 10_000, 10_000, warm=100 * 1000)
    (r,) = report.load_runs(tmp_path)
    assert len(r.rtt_sched_us) == 9_900
    assert report.pct(r.rtt_sched_us, 99.9) == 20.0
    assert abs(report.pct(r.rtt_send_us, 50) - 19.9) < 1e-9
    assert r.checksum_ok and r.lost == 0


def test_tail_and_loss(tmp_path):
    lat = np.full(1000, 10_000, dtype=np.int64)
    lat[-1] = 1_000_000
    write_run(tmp_path / "pinned", "recvmsg", 1000, lat, 1000, 990, same_checksum=False)
    (r,) = report.load_runs(tmp_path)
    assert r.rtt_sched_us.max() == 1000.0
    assert r.lost == 10 and abs(r.loss_pct - 1.0) < 1e-9
    assert not r.checksum_ok
    assert "MISMATCH" in report.latency_table([r])


def test_maxrate_picks_last_rate_before_first_failure(tmp_path):
    d = tmp_path / "maxrate"
    write_run(d, "recvmmsg", 100000, None, 1_000_000, 1_000_000)
    write_run(d, "recvmmsg", 200000, None, 2_000_000, 2_000_000)
    write_run(d, "recvmmsg", 400000, None, 4_000_000, 3_600_000)   # 10% loss
    write_run(d, "recvmmsg", 800000, None, 8_000_000, 8_000_000)   # recovers? still not counted
    table = report.maxrate_table(report.load_runs(tmp_path), 0.01)
    line = [l for l in table.splitlines() if l.startswith("| maxrate | recvmmsg | 200,000")]
    assert line, table
    assert "| 400,000 | 10.00% |" in table


def test_tuning_table_delta(tmp_path):
    write_run(tmp_path / "pinned", "afxdp", 1000, np.full(5000, 10_000, dtype=np.int64), 5000, 5000)
    write_run(tmp_path / "irq", "afxdp", 1000, np.full(5000, 8_000, dtype=np.int64), 5000, 5000)
    t = report.tuning_table(report.load_runs(tmp_path), "pinned")
    assert "-20%" in t and "single run" in t


def test_repetitions_verdict(tmp_path):
    for rep, (b, c) in enumerate([(10_000, 8_000), (10_500, 8_200), (9_800, 7_900)], 1):
        write_run(tmp_path / f"rep{rep}" / "pinned", "afxdp", 1000, np.full(5000, b, dtype=np.int64), 5000, 5000)
        write_run(tmp_path / f"rep{rep}" / "irq", "afxdp", 1000, np.full(5000, c, dtype=np.int64), 5000, 5000)
        write_run(tmp_path / f"rep{rep}" / "noisy", "afxdp", 1000,
                  np.full(5000, [7_000, 12_000, 10_000][rep - 1], dtype=np.int64), 5000, 5000)
    runs = report.load_runs(tmp_path)
    assert {r.rep for r in runs} == {"1", "2", "3"}
    t = report.tuning_table(runs, "pinned")
    assert "better (ranges disjoint)" in t
    assert "within noise" in t
    assert "(9.8\u201310.5)" in report.latency_table(runs)


def test_bad_magic_rejected(tmp_path):
    p = tmp_path / "x.samples.bin"
    p.write_bytes(b"NOTMAGIC" + b"\0" * 8)
    try:
        report.load_samples(p)
    except ValueError:
        return
    raise AssertionError("expected ValueError")


def test_sender_limited_runs_do_not_count(tmp_path):
    d = tmp_path / "maxrate"
    write_run(d, "recvmsg", 100000, None, 1_000_000, 1_000_000)
    write_run(d, "recvmsg", 1000000, None, 4_000_000, 4_000_000)  # tx json says 1M pps achieved...
    tx = d / "recvmsg_1000000.tx.json"
    j = json.loads(tx.read_text()); j["achieved_pps"] = 400000.0; tx.write_text(json.dumps(j))
    table = report.maxrate_table(report.load_runs(tmp_path), 0.01)
    assert "sender-limited" in table
    assert "| maxrate | recvmsg | 100,000 |" in table  # max sustainable stays at the honest run
