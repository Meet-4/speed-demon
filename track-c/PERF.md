# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track C — Join

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 14.44 | 367 | 0 |
| 1 | Correct C baseline: single pass, quote-aware CSV parse, interning, per-country aggregates. Byte-exact sample ×3 + full ×3 — commit `e4ca4ae` | (superseded; board best of that evening) | 0.502 | 85 | **46.82** |
| 2 | Source compression — full ×3 locally 3.59/2.84/2.78 s — commit `dbbf64f` | (superseded) | — | — | — |
| 3 | 12-thread engine: quote-parity-proven record ranges (parallel quote count, step each boundary to the first even-parity newline, so data LFs inside quoted text are never chosen); 16 hash-sharded intern tables (spinlock insert path, lock-free optimistic hits against fixed 8 MB arenas); per-(country, shard) distinct-count bitmaps with atomic bit-set; per-thread country accumulators folded after the join. Byte-exact sample ×3 + full ×3 (0.71/0.98/0.87 s local) — commit `f87d3fc` | **run-e17ed18cddd5** | **0.111** | 84.0 | **55.02** |

Board facts for run-e17ed18cddd5: all gates PASS · T=0.111 s vs ref 14.444 s = **130×** · K=12.60 · S=392 · M=84.0 MB · build 0.53 s.

Failed experiments (they count — this track's debug ladder is the story):

| Attempt | What happened | Resolution |
|---|---|---|
| Arena `realloc` growth | Moved the buffer while lock-free readers were mid-`memcmp` against the old pointer — phantom misses, duplicate ids, nondeterministic counts | Fixed-size 8 MB arenas, exit on overflow (spec-bounded scale) |
| Bitmap indexed by shard-LOCAL id | `bits[c][nid & 15]` mixed one shard's local id into another shard's bitmap — every field right except names_distinct | Intern returns GLOBAL id `(shard << 17) \| local` |
| Plain `bits \|= mask` on shared words | Lost-update race: two concurrent sets into one u64 word can lose one — undercounted distinct names run-to-run | `__atomic_fetch_or`; OR-idempotence keeps everything else order-free |
| A multi-line `/* comment` placed above `exit(3);` | The comment wrapped around the statement and made the arena-copy the if-body — the store only ran when already over capacity (never). Every intern entry published ZERO arena bytes; intern never matched; counts overcounted (ABN 1112 vs Python ground truth 1107). ASan clean, UBSan clean, -O0 same, and a printf perturbation made the output pass — the classic layout-sensitive smell | `gcc -Wall -Wextra` named it: `-Wcomment` ("'/*' within comment"). Comments now stay on their own physical lines; -Wall -Wextra is a standing pre-ship check |

The debugging ladder that cracked it, in order: bisect NTH 12→1 (deterministic-wrong vs nondeterministic separates logic bugs from races) → dump (id, key) pairs and compare per-group set sizes against a Python ground truth over one small group → round-trip-verify the table against its own arena → read the buffer back immediately after the write → store-address probe → finally the field's sharpest tool: build the suspect function verbatim in a 20-line standalone main; it worked there, which indicted the surrounding code, and the compiler warning finished it.

## Notes on method

- The Python ground truth for one country (ABN: 1,120 records / 1,107 distinct names) is what made the overcount visible at all.
- Determinism: identical output hashes on 3 consecutive full runs before every push.
