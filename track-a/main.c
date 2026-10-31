/* Speed Demon 2026 — Track A (Ingest), team ZeroSouls. Zero-dependency C17.
 * /data/requests.log -> /out/summary.txt, byte-identical to the reference.
 * Twelve threads over line-aligned ranges of the mmap. Pass 1 counts per
 * (hour, endpoint) group into per-thread intern tables stored back into each
 * thread's own range struct. The merge interns every endpoint string into
 * one global table in fixed thread order and sums counts through the
 * local->global id maps, so ids and totals are deterministic. Pass 2
 * re-reads the same ranges against the global table (pure lookups by then)
 * and appends each latency to its group slice with an atomic fill counter:
 * the multiset per group is exact, and only the multiset matters because
 * every slice is sorted before the percentiles are read. Group sorts run in
 * parallel over an atomic work counter. Hours are fixed-order; endpoints
 * sort bytewise (prefix first). */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOURS 72
#define MAXEP 4096 /* distinct endpoints (catalogue is 1,842) */
#define EPS 8192   /* interning slots per table, power of two */
#define NTH 12
#define KEY(h, e) (((uint64_t)(h) * MAXEP) + (e))

static __thread char *i_arena;
static __thread uint32_t *i_len, *i_off, *i_slot, *i_cnt, *i_err;
static __thread uint64_t *i_hash;
static __thread int i_nep;

static char *e_arena;
static uint32_t *e_len, *e_off, *e_slot, *g_count, *g_errs;
static uint64_t *e_hash, *g_pos, *g_fill;
static int e_nep;
static uint32_t *g_lat, g_work;

typedef struct { /* one thread's range plus the table it built in pass 1 */
  const char *p0, *p1;
  char *arena;
  uint32_t *len, *off, *slot, *cnt, *err;
  uint64_t *hash;
  int nep;
} range_t;

static uint32_t intern(const char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  uint32_t b = (uint32_t)(h ^ (h >> 29)) & (EPS - 1);
  while (i_slot[b]) {
    uint32_t id = i_slot[b] - 1;
    if (i_hash[id] == h && i_len[id] == len &&
        !memcmp(i_arena + i_off[id], s, len))
      return id;
    b = (b + 1) & (EPS - 1);
  }
  uint32_t id = (uint32_t)i_nep++;
  if (i_nep > MAXEP || i_off[id] + len > (1u << 20))
    exit(3);
  memcpy(i_arena + i_off[id], s, len);
  i_len[id] = len, i_hash[id] = h, i_off[id + 1] = i_off[id] + len,
  i_slot[b] = id + 1;
  return id;
}

/* line := ts(27) SP method SP endpoint SP status(3) SP latency LF */
static const char *parse(const char *p, uint32_t *hour, uint32_t *ep,
                         uint32_t *err, uint32_t *lat) {
  unsigned mo = 10 * (p[5] - '0') + (p[6] - '0');
  unsigned dy = 10 * (p[8] - '0') + (p[9] - '0');
  unsigned hh = 10 * (p[11] - '0') + (p[12] - '0');
  unsigned hi = (mo == 10 ? (dy - 30) * 6 : 30 + (dy - 1) * 24) +
                hh - (mo == 10 && dy == 30 ? 18 : 0);
  const char *s1 = memchr(p + 28, ' ', 8), *s2 = memchr(s1 + 1, ' ', 126),
             *nl = memchr(s2 + 5, '\n', 11);
  uint64_t v = 0;
  for (const char *q = s2 + 5; q < nl; q++)
    v = v * 10 + (uint64_t)(*q - '0');
  if (hi >= HOURS)
    exit(3);
  *hour = hi, *ep = intern(s1 + 1, (uint32_t)(s2 - s1 - 1)),
  *err = (s2[1] == '5'), *lat = (uint32_t)v;
  return nl + 1;
}

static void new_table(void) { /* this thread's private pass-1 table */
  i_arena = malloc(1 << 20);
  i_len = calloc(MAXEP, 4), i_off = calloc(MAXEP + 1, 4),
  i_slot = calloc(EPS, 4), i_hash = calloc(MAXEP, 8),
  i_cnt = calloc((size_t)HOURS * MAXEP, 4),
  i_err = calloc((size_t)HOURS * MAXEP, 4);
  if (!i_arena || !i_len || !i_off || !i_slot || !i_hash || !i_cnt || !i_err)
    exit(2);
  i_nep = 0;
}

static void use_global(void) { /* intern into the merged global table */
  i_arena = e_arena, i_len = e_len, i_off = e_off, i_slot = e_slot,
  i_hash = e_hash, i_nep = e_nep;
}

static void *pass1(void *arg) {
  range_t *r = arg;
  new_table();
  uint32_t h, e, er, la;
  for (const char *p = r->p0; p < r->p1;)
    p = parse(p, &h, &e, &er, &la), i_cnt[KEY(h, e)]++,
    i_err[KEY(h, e)] += er;
  r->arena = i_arena, r->len = i_len, r->off = i_off, r->slot = i_slot,
  r->cnt = i_cnt, r->err = i_err, r->hash = i_hash, r->nep = i_nep;
  return NULL;
}

static void *pass2(void *arg) {
  range_t *r = arg;
  use_global();
  uint32_t h, e, er, la;
  for (const char *p = r->p0; p < r->p1;)
    p = parse(p, &h, &e, &er, &la),
    g_lat[__atomic_fetch_add(&g_fill[KEY(h, e)], 1, __ATOMIC_RELAXED)] = la;
  return NULL;
}

static int u32cmp(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static void *sortwork(void *arg) { /* parallel per-group sorts */
  (void)arg;
  uint32_t total = (uint32_t)HOURS * MAXEP;
  for (uint32_t k; (k = __atomic_fetch_add(&g_work, 64, __ATOMIC_RELAXED)) <
                   total;)
    for (uint32_t j = k; j < k + 64 && j < total; j++)
      if (g_count[j])
        qsort(g_lat + g_pos[j], g_count[j], 4, u32cmp);
  return NULL;
}

static int epcmp(const void *a, const void *b) { /* bytewise, prefix first */
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  uint32_t m = e_len[x] < e_len[y] ? e_len[x] : e_len[y];
  int c = memcmp(e_arena + e_off[x], e_arena + e_off[y], m);
  return c ? c : (e_len[x] < e_len[y] ? -1 : e_len[x] > e_len[y]);
}

static char *fu(char *w, uint32_t v) { /* decimal, no leading zeros */
  char t[10];
  int n = 0;
  do
    t[n++] = (char)('0' + v % 10), v /= 10;
  while (v);
  while (n)
    *w++ = t[--n];
  return w;
}

static pthread_t th[NTH];
static void run12(void *(*fn)(void *), void *args,
                  size_t stride) { /* fire and join NTH */
  for (int t = 0; t < NTH; t++)
    if (pthread_create(&th[t], NULL, fn, (char *)args + t * stride))
      exit(2);
  for (int t = 0; t < NTH; t++)
    pthread_join(th[t], NULL);
}

int main(void) {
  int fd = open("/data/requests.log", O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    return 1;
  int out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    return 1;
  uint64_t size = (uint64_t)st.st_size;
  if (size == 0)
    return close(fd), close(out), 0;
  const char *b = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (b == MAP_FAILED)
    return 1;

  /* NTH line-aligned ranges: each boundary steps past the next newline */
  range_t rng[NTH];
  const char *starts[NTH + 1];
  starts[0] = b, starts[NTH] = b + size;
  for (int t = 1; t < NTH; t++) {
    const char *nl =
        memchr(b + size * t / NTH, '\n', size - size * t / NTH);
    starts[t] = nl ? nl + 1 : starts[NTH];
    if (starts[t] < starts[t - 1])
      starts[t] = starts[t - 1];
  }
  for (int t = 0; t < NTH; t++)
    rng[t] = (range_t){.p0 = starts[t], .p1 = starts[t + 1]};

  e_arena = malloc(1 << 20);
  e_len = calloc(MAXEP, 4), e_off = calloc(MAXEP + 1, 4),
  e_slot = calloc(EPS, 4), e_hash = calloc(MAXEP, 8),
  g_count = calloc((size_t)HOURS * MAXEP, 4),
  g_errs = calloc((size_t)HOURS * MAXEP, 4),
  g_pos = malloc((size_t)HOURS * MAXEP * 8),
  g_fill = calloc((size_t)HOURS * MAXEP, 8);
  if (!e_arena || !e_len || !e_off || !e_slot || !e_hash || !g_count ||
      !g_errs || !g_pos || !g_fill)
    return 2;
  e_nep = 0;

  run12(pass1, rng, sizeof(range_t));

  /* merge: intern every thread's endpoints into the global table in fixed
   * thread order, then sum counts through the local->global id maps */
  use_global();
  uint32_t *map[NTH];
  for (int t = 0; t < NTH; t++) {
    map[t] = malloc((size_t)(rng[t].nep ? rng[t].nep : 1) * 4);
    if (!map[t])
      return 2;
    for (int id = 0; id < rng[t].nep; id++)
      map[t][id] = intern(rng[t].arena + rng[t].off[id], rng[t].len[id]);
  }
  e_nep = i_nep;
  for (int t = 0; t < NTH; t++) {
    for (uint32_t h = 0; h < HOURS; h++)
      for (uint32_t e = 0; e < (uint32_t)rng[t].nep; e++) {
        uint64_t k = KEY(h, map[t][e]);
        g_count[k] += rng[t].cnt[KEY(h, e)];
        g_errs[k] += rng[t].err[KEY(h, e)];
      }
    free(rng[t].len), free(rng[t].off), free(rng[t].slot),
        free(rng[t].hash), free(rng[t].cnt), free(rng[t].err),
        free(rng[t].arena), free(map[t]);
  }

  uint64_t tot = 0;
  for (uint64_t i = 0; i < (uint64_t)HOURS * MAXEP; i++)
    g_pos[i] = g_fill[i] = tot, tot += g_count[i];
  g_lat = malloc(tot ? tot * 4 : 4);
  if (!g_lat)
    return 2;

  run12(pass2, rng, sizeof(range_t));
  run12(sortwork, NULL, 0);

  uint32_t *order = malloc((size_t)(e_nep ? e_nep : 1) * 4);
  if (!order)
    return 2;
  for (uint32_t i = 0; i < (uint32_t)e_nep; i++)
    order[i] = i;
  qsort(order, (size_t)e_nep, 4, epcmp);

  char hs[HOURS][24];
  for (int i = 0, mo = 10, dy = 30, hh = 18; i < HOURS; i++) {
    snprintf(hs[i], 24, "2026-%02d-%02dT%02d:00:00Z", mo, dy, hh);
    if (++hh == 24) {
      hh = 0;
      if (++dy == 32 && mo == 10) /* Oct 31 23:00 is the last hour of Oct */
        dy = 1, mo = 11;
    }
  }

  char *obuf = malloc(16 << 20), *w = obuf; /* full output is ~10.1 MB */
  if (!obuf)
    return 2;
  for (uint32_t h = 0; h < HOURS; h++)
    for (uint32_t k = 0; k < (uint32_t)e_nep; k++) {
      uint32_t id = order[k], n = g_count[KEY(h, id)];
      if (!n)
        continue;
      memcpy(w, hs[h], 20), w += 20, *w++ = ' ';
      memcpy(w, e_arena + e_off[id], e_len[id]), w += e_len[id], *w++ = ' ';
      uint32_t *v = g_lat + g_pos[KEY(h, id)];
      w = fu(w, n), *w++ = ' ';
      w = fu(w, v[(50ULL * n + 99) / 100 - 1]), *w++ = ' ';
      w = fu(w, v[(95ULL * n + 99) / 100 - 1]), *w++ = ' ';
      w = fu(w, v[(99ULL * n + 99) / 100 - 1]), *w++ = ' ';
      w = fu(w, g_errs[KEY(h, id)]), *w++ = ' ';
      w = fu(w, (uint32_t)(g_errs[KEY(h, id)] * 1000000ULL / n)), *w++ = '\n';
    }
  return write(out, obuf, (size_t)(w - obuf)) < 0 ? 1 : 0;
}
