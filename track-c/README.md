# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

One mmap of `records.csv` and a single byte-level pass with a hand-rolled
CSV state machine. BOMs are skipped at record starts only (spec 2.1); quoted
name/note fields are unescaped in place (`""` -> `"`, LF inside quotes is
data, trailing spaces count); `id`/`region_id`/`amount` parse as exact
integers (amounts in cents, no floats); terminators are CRLF or LF per
record. `regions.csv` becomes a direct-index table (region_id -> country).
Per country we keep count, id_sum, amount sum/min/max and unescaped byte
lengths. `names_distinct` is exact, not fingerprinted: every unescaped name
is interned in an FNV-1a open-addressed table and each country holds a
bitmap of the interned ids it has seen. Output is sorted bytewise with
`_UNMATCHED` last (`_` > `Z`), money printed from integer cents. Fixed
capacities cover every scale the generator can emit (spec 2.2, holdout
included) with headroom; input beyond them exits non-zero rather than
guessing. Zero dependencies beyond libc.

## Dependencies

None.

## With another 24 hours

12-way threading over record-aligned byte ranges (the parse is embarrassingly
parallel, the merge is trivial counters + a bitmap OR), SIMD scanning of
quoted-field interiors, and an S-axis pass on main.c.
