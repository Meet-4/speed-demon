# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track C — Join

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 14.44 | 367 | 0 |
| 1 | Correct C baseline: single pass, quote-aware CSV parse, interning, per-country aggregates — commit `e4ca4ae` | (superseded; visible in run history) | 0.502 | 85 | **46.82** |
| 2 | Source compression — commit `dbbf64f` | (superseded) | — | — | — |
| 3 | 12-thread engine: quote-parity-proven record ranges; 16 hash-sharded intern tables (spinlock insert, lock-free optimistic hits against fixed 8 MB arenas); per-(country, shard) bitmaps with atomic bit-set; per-thread accumulators folded after the join. Full ×3 local 0.71/0.98/0.87 s — commit `f87d3fc` | **run-e17ed18cddd5** | **0.112** | 84.0 | **55.02** |
| 4 | Source golf (fire-and-join helper, merged field scans) — commit `e1a7a18` | run at `e1a7a18` | ~0.113 | 84 | 54.68 |
| 5 | Parallel per-country fold (atomic country pull; the 16.7M-word popcount pass spreads over 12 workers). Sample-scale wash (54.36 first score), kept for the full-scale holdout where the fold dominates. Commit `caaa38d` | run at `caaa38d` | ~0.112 | 84 | 54.36 |

Board facts for run-e17ed18cddd5: all gates PASS · T=0.112 s vs ref 14.444 s = **130×** · K=12.60 · S=392 · M=84.0 MB.

Failed experiments (they count — this track's debug ladder is the story):

| Attempt | What happened | Resolution |
|---|---|---|
| Arena `realloc` growth | Moved the buffer while lock-free readers were mid-`memcmp` — phantom misses, duplicate ids, nondeterministic counts | Fixed 8 MB arenas, exit on overflow |
| Bitmap indexed by shard-LOCAL id | `bits[c][nid & 15]` mixed shards — every field right except names_distinct | Intern returns GLOBAL id `(shard << 17) \| local` |
| Plain `bits \|= mask` on shared words | Lost-update race: two concurrent sets into one u64 word lose one — undercounted distinct names run-to-run | `__atomic_fetch_or` |
| Multi-line `/* comment` above `exit(3);` | The comment wrapped around the statement and made the arena-copy the if-body — every intern entry published ZERO arena bytes; intern never matched; ABN counted 1112 vs Python ground truth 1107. ASan clean, UBSan clean, -O0 same, and a printf perturbation made the output pass — the classic layout-sensitive smell | `gcc -Wall -Wextra` named it: `-Wcomment`. Comments stay on their own lines; -Wall -Wextra is a standing pre-ship check |

The debugging ladder that cracked it: bisect NTH 12→1 (deterministic-wrong vs nondeterministic separates logic bugs from races) → dump (id, key) pairs, diff per-group set sizes against a Python ground truth over one small group → round-trip-verify the table against its own arena → read the buffer back right after the write → store-address probe → build the suspect function verbatim in a 20-line standalone main (it worked there — indicting the surrounding code); the compiler warning finished it.

Techniques used (16): direct-index region table (1 byte per region id) · 3-byte country interning with multiply hash · quote-parity-proven record ranges (parallel quote count, step to even-parity LF) · BOM skip at record starts · in-place CSV unescaping (`""` → `"`) · fixed-size arenas (never realloc under lock-free readers) · 16-shard hash-partitioned name interning · spinlock insert + lock-free optimistic probes (append-only entries; stable count = published) · global id encoding `(shard << 17) \| local` · per-(country, shard) bitmaps with atomic fetch_or · per-thread accumulators with post-join fold · parallel per-country fold via atomic pull · exact popcount distinct counting · integer cents arithmetic (no floats) · bytewise country sort, prefix-first · minimal-output decimal formatters.
