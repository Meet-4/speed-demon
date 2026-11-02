# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track D — Traverse

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 72.69 | 893 | 0 |
| 1 | Correct C baseline: CSR adjacency, forward Dijkstra per query (two-pass build, no edge-list staging) — commit `92bd603` | (superseded) | ~5.97 | ~305 | **32.49** |
| 2 | Source compression — three bugs self-caught before push (loop counter `u`, `p = hdr` init, 32-bit `hv` type) — commit `7c1bcac` | (superseded) | — | — | **33.48** |
| 3 | 12 query workers: duplicate query lines folded before dispatch (first occurrence wins, open-addressing cache), workers pull query indices via `__atomic_fetch_add`, per-thread dist/stamp/heap, answers written by query index and printed in order. Full ×3 local 2.0–2.4 s (was 6.1 s) — commit `01d434a` | **run-00a378773afa** | 1.279 | 305.0 | **43.20** |
| 4 | Memory trim: per-thread stamp+dist packed into one u64 (`gen << 53 \| dist`, disjointness proven: simple-path sum < n·(2³²−1) < 2⁵³, gen ≤ 2047 with memset guard); workers 12 → 6 (measured trade: 6T modelled K 8.37 vs 12T 7.82). Full ×3 local 2.61–2.86 s — commit `0082d9b` | **run-dc886a16b986** | **1.353** | **135.1** | **46.03** |

Board facts for run-dc886a16b986: all gates PASS · T=1.353 s vs ref 72.693 s = **54×** · K=8.33 · S=227 · M=135.1 MB (−56% from step 3's 305.0).

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| Parallel CSR parse with relaxed atomics (both passes over line-aligned edge chunks) | Green but SLOWER: best local full 4.24 s vs 2.61 s serial — atomic degree/scatter counters serialize on hub nodes (every edge to a high-degree node contends one cache line). Reverted before any push | Kept the serial two-pass build; documented as the cost of atomic histograms on scale-free graphs |
| A stale sample hash in internal notes (`a4c76ed0441b…`) | The gate flagged a mismatch that was never a code bug — real sample hash is `a4c76ed030bc…` | Notes corrected; the spec's published hashes are the only truth |
| Post-increment atomic for the work counter | Track A's incident (post-increment shifts every slot) was already documented — this track uses `__atomic_fetch_add` from the start | Recorded as the lesson applied before it bit |

Techniques used (12): mmap graph + two-pass CSR (degree histogram → scatter, zero staging memory) · packed stamp+dist u64 with proven bit disjointness · generation wipe guard at 2048 · on-demand binary heap (sift-up/down) · stale-entry skip via dist comparison · `s==t` / zero-out-degree / zero-in-degree early exits · duplicate-query folding pre-dispatch (open addressing, first occurrence) · atomic work-counter query dispatch · per-thread search state (no shared mutable state at query time) · answers by query index, printed in order · measured 6-vs-12-worker M/T trade · 64-bit distances (parallel edges need no handling — only the lightest can improve).

Method note: per-worker dist arrays dominate M (1.74M × 8 B each); T is sqrt-weighted against a 72.7 s reference, so a measured +6% T for −56% M was the right trade — the board confirmed it +2.8 points.
