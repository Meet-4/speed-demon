/* Speed Demon 2026 — Track D (Traverse) — team ZeroSouls. Zero-dependency C17.
 * /data/graph.txt + /data/queries.txt -> /out/distances.txt, one line per
 * query: shortest directed distance s->t, or -1. Both inputs are mmap'd (the
 * board pre-warms /data). Two passes over the graph bytes build the CSR: a
 * degree histogram, then a scatter — no raw edge list is stored. Duplicate
 * query lines are folded first (open-addressing cache keeps the index of the
 * FIRST occurrence; duplicates print that answer). The unique queries are
 * solved by 6 threads pulling work from an atomic counter; each thread owns
 * its dist/stamp state and its own on-demand heap, so nothing is shared
 * except the read-only CSR. Half the workers of the last build: the
 * per-worker arrays dominate M, and the tail of 12 simultaneous searches
 * wasted the extra threads anyway. The generation stamp and the
 * u64 per node: stamp in the top 11 bits, dist in the low 53 — provably
 * disjoint, since a stored dist is a simple-path sum < n*(2^32-1) < 2^53 and
 * gen < 2048 (a memset guard makes even that bound unnecessary). Fast answers
 * never start a search: s==t is 0; with s!=t, a source with no out-edges or a
 * target with no in-edges is -1. Distances are 64-bit: a path sums up to
 * (n-1)*(2^32-1). */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NTH 6

static uint32_t n, *off, *ev, *ew; /* CSR, read-only during queries */
static unsigned char *has_in;

/* per-thread search state: nothing else is mutable at query time. cd packs
 * the generation stamp (top 11 bits) with the distance (low 53 bits) into
 * one u64, replacing a separate stamp array: a stored dist is always the
 * length of a simple path, hence < n*(2^32-1) < 2^53, so the fields never
 * overlap and every comparison is the plain 64-bit one. */
#define LOW53 ((1ULL << 53) - 1)
static __thread uint32_t hn, hcap, gen;
static __thread uint64_t *cd, *hd;
static __thread uint32_t *hv;

/* Parse an unsigned decimal; the single byte after the digits is the field
 * separator (space, or newline on the last field of a line). */
static const char *num(const char *p, uint64_t *v) {
  uint64_t x = 0;
  while (*p >= '0')
    x = x * 10 + (uint64_t)(*p++ - '0');
  *v = x;
  return p + 1;
}

static void hpush(uint64_t d, uint32_t v) {
  if (hn == hcap) {
    hcap = hcap ? hcap * 2 : 1 << 12;
    hd = realloc(hd, (size_t)hcap * 8);
    hv = realloc(hv, (size_t)hcap * 4);
    if (!hd || !hv)
      exit(2);
  }
  uint32_t i = hn++;
  while (i) {
    uint32_t up = (i - 1) / 2;
    if (hd[up] <= d)
      break;
    hd[i] = hd[up];
    hv[i] = hv[up];
    i = up;
  }
  hd[i] = d;
  hv[i] = v;
}

/* Remove the minimum; caller guarantees hn > 0. Returns its key, sets *v. */
static uint64_t hpop(uint32_t *v) {
  uint64_t d = hd[0];
  uint32_t k = --hn, i = 0;
  *v = hv[0];
  uint64_t ld = hd[k];
  uint32_t lv = hv[k];
  for (;;) {
    uint32_t c = 2 * i + 1;
    if (c >= k)
      break;
    if (c + 1 < k && hd[c + 1] < hd[c])
      c++;
    if (hd[c] >= ld)
      break;
    hd[i] = hd[c];
    hv[i] = hv[c];
    i = c;
  }
  hd[i] = ld;
  hv[i] = lv;
  return d;
}

/* d(s, t) by Dijkstra, or -1 if unreachable. Every pushed node carries the
 * current gen in its stamp field, so a heap entry is stale exactly when its
 * key exceeds the node's current dist. Parallel edges need no handling: only
 * the lightest can ever improve a distance. */
static int64_t solve(uint32_t s, uint32_t t) {
  if (s == t)
    return 0;
  if (off[s + 1] == off[s] || !has_in[t]) /* no first edge, no last edge */
    return -1;
  gen++;
  if (gen == 1u << 11) { /* stamp space exhausted: wipe it, restart epochs */
    memset(cd, 0, (size_t)n * 8);
    gen = 1;
  }
  hn = 0, cd[s] = (uint64_t)gen << 53, hpush(0, s);
  while (hn) {
    uint32_t v;
    uint64_t d = hpop(&v);
    if (d > (cd[v] & LOW53)) /* v carries gen: it was stamped at push time */
      continue;
    if (v == t)
      return (int64_t)d;
    for (uint32_t i = off[v]; i < off[v + 1]; i++) {
      uint64_t nd = d + ew[i];
      uint32_t to = ev[i];
      uint64_t c = cd[to];
      if ((c >> 53) != gen || nd < (c & LOW53))
        cd[to] = ((uint64_t)gen << 53) | nd, hpush(nd, to);
    }
  }
  return -1;
}

static uint32_t nq, *qs, *qt, *qfirst, work;
static int64_t *ans;

static void *qworker(void *arg) {
  (void)arg;
  cd = calloc((size_t)n, 8);
  hcap = 1 << 12, hn = 0, gen = 0;
  hd = malloc((size_t)hcap * 8);
  hv = malloc((size_t)hcap * 4);
  if (!cd || !hd || !hv)
    exit(2);
  for (;;) {
    uint32_t i = __atomic_fetch_add(&work, 1, __ATOMIC_RELAXED);
    if (i >= nq)
      break;
    if (qfirst[i] == i) /* first occurrence: solve it */
      ans[i] = solve(qs[i], qt[i]);
  }
  return NULL;
}

int main(void) {
  struct stat st;
  int fd = open("/data/graph.txt", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    return 1;
  const char *g = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (g == MAP_FAILED)
    return 1;
  uint64_t nn, mm, u, v, w;
  const char *p, *hdr = num(num(g, &nn), &mm); /* header: n SP m LF */
  n = (uint32_t)nn;
  off = calloc(n + 1, 4), ev = malloc(mm ? (size_t)mm * 4 : 4),
  ew = malloc(mm ? (size_t)mm * 4 : 4), has_in = calloc(n, 1);
  if (!off || !ev || !ew || !has_in)
    return 2;
  {
    uint32_t *cur = malloc((size_t)(n + 1) * 4);
    if (!cur)
      return 2;
    p = hdr;
    for (uint64_t i = 0; i < mm; i++) /* pass 1: degrees */
      p = num(num(num(p, &u), &v), &w), off[u + 1]++, has_in[v] = 1;
    for (u = 0; u < n; u++)
      off[u + 1] += off[u];
    memcpy(cur, off, (size_t)(n + 1) * 4);
    p = hdr;
    for (uint64_t i = 0; i < mm; i++) { /* pass 2: scatter into the CSR */
      p = num(num(num(p, &u), &v), &w);
      uint32_t c = cur[u]++;
      ev[c] = (uint32_t)v, ew[c] = (uint32_t)w;
    }
    free(cur);
  }

  int qfd = open("/data/queries.txt", O_RDONLY);
  if (qfd < 0 || fstat(qfd, &st))
    return 1;
  uint32_t qcap = 256;
  qs = malloc((size_t)qcap * 4), qt = malloc((size_t)qcap * 4);
  if (!qs || !qt)
    return 2;
  nq = 0;
  if (st.st_size) {
    const char *qp = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE,
                          qfd, 0),
               *qe;
    if (qp == MAP_FAILED)
      return 1;
    for (qe = qp + st.st_size; qp < qe;) {
      uint64_t s, t;
      qp = num(num(qp, &s), &t);
      if (nq == qcap) {
        qcap *= 2;
        qs = realloc(qs, (size_t)qcap * 4);
        qt = realloc(qt, (size_t)qcap * 4);
        if (!qs || !qt)
          return 2;
      }
      qs[nq] = (uint32_t)s, qt[nq] = (uint32_t)t, nq++;
    }
  }

  /* fold duplicates: qfirst[i] = index of the first occurrence of query i */
  uint32_t ccap = 1 << 12;
  while (ccap < 4 * nq)
    ccap <<= 1;
  uint64_t *ck = calloc(ccap, 8);
  uint32_t *cv = malloc((size_t)ccap * 4);
  qfirst = malloc((size_t)(nq ? nq : 1) * 4);
  ans = malloc((size_t)(nq ? nq : 1) * 8);
  if (!ck || !cv || !qfirst || !ans)
    return 2;
  for (uint32_t i = 0; i < nq; i++) {
    uint64_t key = ((uint64_t)qs[i] << 32) | qt[i];
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) & (ccap - 1);
    while (ck[h] && ck[h] != key + 1)
      h = (h + 1) & (ccap - 1);
    if (ck[h])
      qfirst[i] = cv[h]; /* duplicate: reuse the first occurrence */
    else
      ck[h] = key + 1, cv[h] = i, qfirst[i] = i;
  }

  work = 0;
  pthread_t th[NTH];
  for (int t = 0; t < NTH; t++)
    if (pthread_create(&th[t], NULL, qworker, NULL))
      return 2;
  for (int t = 0; t < NTH; t++)
    pthread_join(th[t], NULL);

  char *obuf = malloc((size_t)nq * 21 + 8), *ow = obuf;
  if (!obuf)
    return 2;
  for (uint32_t i = 0; i < nq; i++) {
    int64_t a = ans[qfirst[i]];
    if (a < 0) {
      memcpy(ow, "-1\n", 3);
      ow += 3;
    } else {
      char tmp[20];
      int k = 0;
      uint64_t x = (uint64_t)a;
      do {
        tmp[k++] = (char)('0' + x % 10);
        x /= 10;
      } while (x);
      while (k)
        *ow++ = tmp[--k];
      *ow++ = '\n';
    }
  }
  int out = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return out < 0 || write(out, obuf, (size_t)(ow - obuf)) < 0 ? 1 : 0;
}
