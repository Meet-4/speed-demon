/* Speed Demon 2026 — Track C (Join), team ZeroSouls. Zero-dependency C17.
 * /data/records.csv + /data/regions.csv -> /out/aggregates.txt, byte-identical
 * to the reference. regions.csv is parsed first into a direct-index table
 * (region_id -> country). The records pass is twelve threads over
 * quote-parity-proven record ranges: a parallel count of quote bytes fixes
 * the parity at every range boundary, and each boundary steps from there to
 * the first LF at even parity — a data LF inside a quoted field is never
 * chosen — so every record is parsed by exactly one thread. Country
 * accumulators are per-thread and folded afterwards. Names go into sixteen
 * hash-sharded intern tables, each with its own arena and a spinlock that
 * serializes only the insert path; probes read entries optimistically
 * (entries are append-only, so a stable count means fully published), and a
 * torn read falls back to the lock. Name ids are assigned in race order by
 * design — they never appear in the output: names_distinct is the popcount
 * of per-(country, shard) bitmaps, and concurrent set-bits are benign OR
 * races. BOMs are skipped at record starts only, quoted fields are unescaped
 * in place ("" -> ", LF inside quotes is data), numerics are exact
 * integers, no floats anywhere. */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXC 512 /* countries (spec: 240) plus _UNMATCHED */
#define RTAB (1u << 24) /* region table; ids stay under 10^7 (2.2) */
#define NSHARD 16 /* name intern shards; power of two */
#define SSLOT (1u << 17) /* slots per shard; <=2^17 names total */
#define NTH 12

static uint32_t nc; /* interned country codes */
static char ccodes[MAXC][4];
static uint32_t code_slot[1024];
static uint8_t *rtab;
static uint64_t (*bits)[NSHARD][SSLOT / 64]; /* per (country, shard) */

typedef struct { /* one shard of the name table */
  unsigned char lock;
  uint32_t *slot; /* name id + 1 per slot, 0 = empty */
  uint64_t count, bytes; /* ids given out so far; arena bytes in use */
  uint64_t *eh; /* entry hash, by id */
  uint32_t *eo, *el; /* entry arena offset / length, by id */
  char *arena;
  uint64_t acap; /* arena capacity */
} shard;
static shard sh[NSHARD];

/* per-thread country accumulators, folded after the pass */
typedef struct {
  uint64_t cnt, ids, sum, nb, ob;
  int64_t mn, mx;
} acc;
static acc t_acc[NTH][MAXC];
static uint64_t qcount[NTH];

static __thread char *scr; /* scratch for one unescaped field */
static __thread uint32_t scr_cap;

static char *xgrow(char *p, uint32_t *cap, uint32_t need) {
  uint32_t c = *cap ? *cap * 2 : 4096;
  while (c < need)
    c *= 2;
  return (*cap = c, realloc(p, c));
}

/* id of the exact byte string s[0..len) in its shard. Probes are lock-free:
 * entries are append-only, so a stable count means the entry was fully
 * published. A torn read, or an empty slot, falls back to the lock, where
 * the answer is definitive and inserts are serialized. */
static uint32_t intern(const unsigned char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h ^= s[i], h *= 1099511628211ULL;
  uint32_t kd = (uint32_t)(h & (NSHARD - 1));
  shard *k = &sh[kd];
  uint32_t b = (uint32_t)(h >> 24) & (SSLOT - 1);
  for (;;) {
    uint32_t id = k->slot[b];
    if (id) {
      uint64_t c0 = k->count;
      if (k->eh[id - 1] == h && k->el[id - 1] == len &&
          !memcmp(k->arena + k->eo[id - 1], s, len))
        return (kd << 17) | (id - 1);
      if (k->count == c0) { /* entry complete and different: probe on */
        b = (b + 1) & (SSLOT - 1);
        continue;
      }
    }
    break; /* empty slot, or a publish is racing: take the lock */
  }
  while (__atomic_test_and_set(&k->lock, __ATOMIC_ACQUIRE))
    while (__atomic_load_n(&k->lock, __ATOMIC_RELAXED))
      ;
  for (b = (uint32_t)(h >> 24) & (SSLOT - 1);; b = (b + 1) & (SSLOT - 1)) {
    uint32_t id = k->slot[b];
    if (id) {
      if (k->eh[id - 1] == h && k->el[id - 1] == len &&
          !memcmp(k->arena + k->eo[id - 1], s, len)) {
        __atomic_clear(&k->lock, __ATOMIC_RELEASE);
        return (kd << 17) | (id - 1);
      }
      continue;
    }
    uint32_t nid = (uint32_t)k->count++;
    if (k->count > SSLOT / 2)
      exit(3); /* beyond every scale the generator emits */
    if (k->bytes + len > k->acap) /* spec-bounded: see above */
      exit(3);
    memcpy(k->arena + k->bytes, s, len);
    k->eh[nid] = h, k->eo[nid] = (uint32_t)k->bytes, k->el[nid] = len;
    k->bytes += len;
    k->slot[b] = nid + 1; /* publish: after the entry and the arena */
    __atomic_clear(&k->lock, __ATOMIC_RELEASE);
    return (kd << 17) | nid;
  }
}

/* quoted field at p (opening quote): unescape into scr when copy is set,
 * always count the unescaped bytes; returns p past the closing quote and
 * the comma that follows it. "" -> ", every other byte is literal. */
static const unsigned char *qfield(const unsigned char *p, int copy,
                                   uint32_t *lenp) {
  uint32_t w = 0;
  for (p++;;) {
    const unsigned char *s = p;
    while (*p != '"')
      p++; /* every quoted field in a valid file is closed */
    uint32_t l = p[1] == '"' ? (uint32_t)(p + 1 - s) : (uint32_t)(p - s);
    if (copy) {
      if (w + l > scr_cap)
        scr = xgrow(scr, &scr_cap, w + l);
      memcpy(scr + w, s, l);
    }
    w += l;
    if (p[1] == '"') {
      p += 2;
      continue;
    }
    if (p[1] != ',')
      exit(3); /* a closing quote is followed by the separator */
    return *lenp = w, p + 2;
  }
}

/* one record at p (id,name,note,region_id,amount): accumulates into a and
 * sets the name bit. Returns p past the cents field (before the LF). */
static const unsigned char *record(const unsigned char *p, acc *a) {
  uint64_t id = 0, reg = 0, cents = 0;
  while (*p != ',')
    id = id * 10 + (*p++ - '0');
  p++;
  const unsigned char *nm;
  uint32_t nml, ntl;
  if (*p == '"') { /* name: quoted (the usual case) or empty unquoted */
    p = qfield(p, 1, &nml);
    nm = (const unsigned char *)scr;
  } else {
    for (nm = p; *p != ',';)
      p++;
    nml = (uint32_t)(p - nm);
    p++;
  }
  if (*p == '"')
    p = qfield(p, 0, &ntl); /* note: only its length matters */
  else {
    const unsigned char *s = p;
    while (*p != ',')
      p++;
    ntl = (uint32_t)(p - s);
    p++;
  }
  while (*p != ',')
    reg = reg * 10 + (*p++ - '0');
  p++;
  unsigned neg = *p == '-';
  p += neg;
  while (*p >= '0' && *p <= '9')
    cents = cents * 10 + (*p++ - '0');
  if (*p != '.')
    exit(3);
  cents = cents * 100 + (uint64_t)(p[1] - '0') * 10 + (uint64_t)(p[2] - '0');
  p += 3;
  uint32_t c = (reg < RTAB && rtab[reg]) ? (uint32_t)(rtab[reg] - 1) : nc;
  int64_t amt = neg ? -(int64_t)cents : (int64_t)cents;
  a[c].cnt++, a[c].ids += id, a[c].sum += amt, a[c].nb += nml,
      a[c].ob += ntl;
  if (amt < a[c].mn)
    a[c].mn = amt;
  if (amt > a[c].mx)
    a[c].mx = amt;
  uint32_t gid = intern(nm, nml);
  __atomic_fetch_or(&bits[c][gid >> 17][(gid >> 6) & (SSLOT / 64 - 1)],
                    1ull << (gid & 63), __ATOMIC_RELAXED);
  return p;
}

typedef struct {
  const unsigned char *p0, *p1;
  int t;
} range;

static void *worker(void *arg) {
  range *r = arg;
  acc *a = t_acc[r->t];
  const unsigned char *p = r->p0, *end = r->p1;
  while (p < end) {
    if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
      p += 3; /* a BOM is only legal at a record start */
    p = record(p, a);
    if (p < end) { /* CRLF or LF; the file always ends with one */
      if (*p == '\r')
        p += 2;
      else if (*p == '\n')
        p++;
      else
        exit(3);
    }
  }
  return NULL;
}

static void *qcountwork(void *arg) { /* quote bytes in a fixed byte range */
  range *r = arg;
  uint64_t n = 0;
  const unsigned char *p = r->p0;
  while (p < r->p1) {
    p = memchr(p, '"', (size_t)(r->p1 - p));
    if (!p)
      break;
    n++, p++;
  }
  qcount[r->t] = n;
  return NULL;
}

static int keycmp(const void *a, const void *b) { /* bytewise on the key */
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  const char *kx = x < nc ? ccodes[x] : "_UNMATCHED";
  const char *ky = y < nc ? ccodes[y] : "_UNMATCHED";
  int c = memcmp(kx, ky, 3);
  uint32_t lx = x < nc ? 3 : 10, ly = y < nc ? 3 : 10;
  return c ? c : lx < ly ? -1 : lx > ly;
}

static char *fu(char *w, uint64_t v) { /* decimal, no leading zeros */
  char t[20];
  int n = 0;
  do
    t[n++] = (char)('0' + v % 10), v /= 10;
  while (v);
  while (n)
    *w++ = t[--n];
  return w;
}

static char *fc(char *w, int64_t c) { /* cents: -525 -> -5.25, 0 -> 0.00 */
  if (c < 0)
    *w++ = '-';
  uint64_t a = c < 0 ? (uint64_t)(-(c + 1)) + 1 : (uint64_t)c;
  w = fu(w, a / 100);
  *w++ = '.';
  *w++ = (char)('0' + a / 10 % 10);
  *w++ = (char)('0' + a % 10);
  return w;
}

int main(void) {
  int fd = open("/data/regions.csv", O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    return 1;
  uint64_t rsz = (uint64_t)st.st_size;
  unsigned char *rr = malloc(rsz);
  if (!rr)
    return 1;
  for (uint64_t got = 0; got < rsz;) {
    ssize_t k = read(fd, rr + got, rsz - got);
    if (k <= 0)
      return 1;
    got += (uint64_t)k;
  }
  rtab = calloc(RTAB, 1); /* ids <= 999,999 (2.3): ~1 MB touched */
  const unsigned char *rp = rr;
  while (*rp++ != '\n')
    ; /* plain header line: no quotes, no BOM */
  while (rp < rr + rsz) {
    uint32_t rid = 0;
    while (*rp >= '0' && *rp <= '9')
      rid = rid * 10 + (*rp++ - '0');
    if (*rp++ != ',')
      exit(3);
    uint32_t b = (((uint32_t)rp[0] << 16) | ((uint32_t)rp[1] << 8) | rp[2]) *
                     2654435761u >> 22;
    while (code_slot[b] && memcmp(ccodes[code_slot[b] - 1], rp, 3))
      b = (b + 1) & 1023; /* intern the 3-byte country code */
    if (!code_slot[b]) {
      if (nc >= MAXC - 1)
        exit(3);
      memcpy(ccodes[nc], rp, 3);
      code_slot[b] = ++nc;
    }
    rp += 3;
    if (*rp++ != '\n' || rid >= RTAB)
      exit(3);
    rtab[rid] = code_slot[b]; /* duplicates, if any: last wins, like a dict */
  }

  fd = open("/data/records.csv", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    return 1;
  uint64_t size = (uint64_t)st.st_size;
  const unsigned char *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (base == MAP_FAILED)
    return 1;
  const unsigned char *p = base;
  if (size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
    p += 3;
  static const char HDR[] = "id,name,note,region_id,amount";
  if ((size_t)(base + size - p) < sizeof HDR - 1 ||
      memcmp(p, HDR, sizeof HDR - 1) ||
      !(p = memchr(p, '\n', base + size - p)))
    exit(3); /* one plain header line */
  p++;

  bits = calloc((size_t)MAXC * NSHARD * (SSLOT / 64), 8);
  for (uint32_t i = 0; i < NSHARD; i++) {
    sh[i].slot = calloc(SSLOT, 4);
    sh[i].eh = malloc((size_t)(SSLOT / 2) * 8);
    sh[i].eo = malloc((size_t)(SSLOT / 2) * 4);
    sh[i].el = malloc((size_t)(SSLOT / 2) * 4);
    sh[i].arena = malloc(8u << 20), sh[i].acap = 8u << 20;
    if (!sh[i].slot || !sh[i].eh || !sh[i].eo || !sh[i].el || !sh[i].arena)
      return 2;
  }

  /* fixed byte ranges, only for the quote count */
  range qr[NTH];
  pthread_t th[NTH];
  for (int t = 0; t < NTH; t++) {
    qr[t] = (range){base + size * t / NTH, base + size * (t + 1) / NTH, t};
    if (pthread_create(&th[t], NULL, qcountwork, &qr[t]))
      return 2;
  }
  for (int t = 0; t < NTH; t++)
    pthread_join(th[t], NULL);

  /* record ranges: from each boundary, step to the first LF at even quote
   * parity (the running sum of quote bytes is the parity) */
  const unsigned char *starts[NTH + 1];
  starts[0] = p, starts[NTH] = base + size;
  for (int t = 1; t < NTH; t++) {
    uint64_t par = 0;
    for (int u = 0; u < t; u++)
      par += qcount[u];
    const unsigned char *q = base + size * t / NTH;
    if (q < p) /* the header region has no quotes: parity unchanged */
      q = p;
    for (;;) {
      const unsigned char *nl =
          memchr(q, '\n', (size_t)(starts[NTH] - q));
      const unsigned char *qq = memchr(q, '"', (size_t)(starts[NTH] - q));
      if (!nl) {
        q = starts[NTH]; /* unreachable: records are well-formed */
        break;
      }
      if (qq && qq < nl) { /* quotes before the LF: toggle, step past */
        par ^= 1;
        q = qq + 1;
        continue;
      }
      if (!(par & 1)) { /* LF at even parity: a record boundary */
        q = nl + 1;
        break;
      }
      q = nl + 1; /* data LF inside a quoted field: keep walking */
    }
    starts[t] = q;
    if (starts[t] < starts[t - 1])
      starts[t] = starts[t - 1];
  }

  for (uint32_t i = 0; i <= nc; i++)
    for (int t = 0; t < NTH; t++)
      t_acc[t][i].mn = INT64_MAX, t_acc[t][i].mx = INT64_MIN;
  range rr2[NTH];
  for (int t = 0; t < NTH; t++) {
    rr2[t] = (range){starts[t], starts[t + 1], t};
    if (pthread_create(&th[t], NULL, worker, &rr2[t]))
      return 2;
  }
  for (int t = 0; t < NTH; t++)
    pthread_join(th[t], NULL);

  uint64_t g_cnt[MAXC], g_ids[MAXC], g_dist[MAXC], g_nb[MAXC], g_ob[MAXC];
  int64_t g_sum[MAXC], g_min[MAXC], g_max[MAXC];
  for (uint32_t c = 0; c <= nc; c++) {
    g_cnt[c] = g_ids[c] = g_dist[c] = g_nb[c] = g_ob[c] = 0;
    g_sum[c] = 0, g_min[c] = INT64_MAX, g_max[c] = INT64_MIN;
    for (int t = 0; t < NTH; t++) {
      acc *a = &t_acc[t][c];
      g_cnt[c] += a->cnt, g_ids[c] += a->ids, g_sum[c] += a->sum,
          g_nb[c] += a->nb, g_ob[c] += a->ob;
      if (a->mn < g_min[c])
        g_min[c] = a->mn;
      if (a->mx > g_max[c])
        g_max[c] = a->mx;
    }
    for (uint32_t s = 0; s < NSHARD; s++) /* distinct names: exact popcount */
      for (uint32_t w = 0; w < SSLOT / 64; w++)
        g_dist[c] += (uint64_t)__builtin_popcountll(bits[c][s][w]);
  }

  uint32_t order[MAXC], n = 0;
  for (uint32_t i = 0; i <= nc; i++)
    order[n++] = i;
  qsort(order, n, sizeof *order, keycmp);
  char *obuf = malloc(64 << 10), *w = obuf; /* full output is ~17 KB */
  if (!obuf)
    return 2;
  for (uint32_t k = 0; k < n; k++) {
    uint32_t i = order[k];
    if (!g_cnt[i])
      continue; /* a country with no records produces no line */
    uint32_t kl = i < nc ? 3 : 10;
    memcpy(w, i < nc ? ccodes[i] : "_UNMATCHED", kl);
    w += kl, *w++ = ' ';
    w = fu(w, g_cnt[i]), *w++ = ' ';
    w = fu(w, g_ids[i]), *w++ = ' ';
    w = fc(w, g_sum[i]), *w++ = ' ';
    w = fc(w, g_min[i]), *w++ = ' ';
    w = fc(w, g_max[i]), *w++ = ' ';
    w = fu(w, g_dist[i]), *w++ = ' ';
    w = fu(w, g_nb[i]), *w++ = ' ';
    w = fu(w, g_ob[i]), *w++ = '\n';
  }
  int out = open("/out/aggregates.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return out < 0 || write(out, obuf, (size_t)(w - obuf)) < 0 ? 1 : 0;
}
