# Speed Demon 2026 — ZeroSouls

Team **ZeroSouls** — Speed Demon 2026, all four tracks: **A (Ingest)**, **B (Index)**, **C (Join)** and **D (Traverse)**.

Every solution here is **zero-dependency C** (C17, `gcc -O3 -march=native -pthread`), built and run exactly as the scoring harness does: build `FROM gcc:16`, runtime `FROM debian:13-slim`, no network at build, one output file to `/out`.

## Tracks

| Track | Task | Reference | Ours (target) |
|---|---|---|---|
| A · Ingest | 3.1M-line HTTP log → per-(hour, endpoint) latency stats | 32.05 s | 0.78 s (41×, board-verified) |
| B · Index | 254 MB of documents → ranked top-k search over 50k queries | 56.35 s | in progress |
| C · Join | 204 MB hostile CSV → joined per-country aggregates | 14.44 s | 0.50 s (29×, board-verified) |
| D · Traverse | 5M-edge graph, 2k shortest-path queries | 72.69 s | 6.3 s (12×, board-verified) |

All outputs are **byte-identical** to the reference (`sha256` gate). No third-party libraries, no precomputation, no hidden indexes: everything is computed from the input at run time, inside the timed run.

## Build & run

```
docker build -t sd-a track-a
docker run --rm -v <data>:/data:ro -v <out>:/out:rw -e TRACK=a sd-a
```

The container reads `/data`, writes exactly one file to `/out`, exits 0. It runs as an unprivileged user, with no network.

## Method

See `PERF.md` for the full optimisation log: every step, every measurement, every board run ID. Principles:

- correct first, byte-exact against the reference before any speed work;
- measure on the same contract as the board (same image digests, same sandbox flags);
- small memory, tight source, zero dependencies — the efficiency coefficient prices all four axes.

## Licence

MIT — see `LICENSE`.
