/* Speed Demon 2026 — Track D (Traverse) — team ZeroSouls. Zero-dependency C17.
 * /data/graph.txt + /data/queries.txt -> /out/distances.txt, one line per
 * query: shortest directed distance s->t, or -1. Both inputs are mmap'd (the
 * board pre-warms /data). Two passes over the graph bytes build the CSR: a
 * degree histogram, then a scatter — no raw edge list is stored. Each query
 * runs one Dijkstra over the CSR with a lazy binary min-heap and one
 * generation-stamped dist array, so nothing is reset between queries. Three
 * answers never start a search: s==t is 0; with s!=t, a source with no
 * out-edges or a target with no in-edges is -1. Duplicate query lines reuse
 * the first answer from an open-addressing cache. Distances are 64-bit: a
 * path sums up to (n-1)*(2^32-1), which overflows 32 bits, not 63. */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t n, *off, *cur, *ev, *ew, *stamp, gen, hn, hcap;
static uint64_t *dist, *hd;
static uint32_t *hv;
static unsigned char *has_in;

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

/* d(s, t) by Dijkstra, or -1 if unreachable. Every pushed node carries
 * stamp == gen, so a heap entry is stale exactly when its key exceeds the
 * node's current dist. Parallel edges need no handling: only the lightest
 * can ever improve a distance. */
static int64_t solve(uint32_t s, uint32_t t) {
  if (s == t)
    return 0;
  if (off[s + 1] == off[s] || !has_in[t]) /* no first edge, no last edge */
    return -1;
  gen++, hn = 0, dist[s] = 0, stamp[s] = gen, hpush(0, s);
  while (hn) {
    uint32_t v;
    uint64_t d = hpop(&v);
    if (d > dist[v])
      continue;
    if (v == t)
      return (int64_t)d;
    for (uint32_t i = off[v]; i < off[v + 1]; i++) {
      uint64_t nd = d + ew[i];
      uint32_t to = ev[i];
      if (stamp[to] != gen || nd < dist[to])
        stamp[to] = gen, dist[to] = nd, hpush(nd, to);
    }
  }
  return -1;
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
  off = calloc(n + 1, 4), cur = malloc((size_t)(n + 1) * 4), has_in = calloc(n, 1), dist = malloc((size_t)n * 8), stamp = calloc(n, 4), ev = malloc(mm ? (size_t)mm * 4 : 4), ew = malloc(mm ? (size_t)mm * 4 : 4);
  if (!off || !cur || !has_in || !dist || !stamp || !ev || !ew)
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
  int qfd = open("/data/queries.txt", O_RDONLY);
  if (qfd < 0 || fstat(qfd, &st))
    return 1;
  uint32_t q = 0, qcap = 256;
  uint32_t *qs = malloc((size_t)qcap * 4), *qt = malloc((size_t)qcap * 4);
  if (!qs || !qt)
    return 2;
  if (st.st_size) {
    const char *qp = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, qfd, 0), *qe;
    if (qp == MAP_FAILED)
      return 1;
    for (qe = qp + st.st_size; qp < qe;) {
      uint64_t s, t;
      qp = num(num(qp, &s), &t);
      if (q == qcap) {
        qcap *= 2;
        qs = realloc(qs, (size_t)qcap * 4);
        qt = realloc(qt, (size_t)qcap * 4);
        if (!qs || !qt)
          return 2;
      }
      qs[q] = (uint32_t)s, qt[q] = (uint32_t)t, q++;
    }
  }
  uint32_t ccap = 1 << 12; /* duplicate-query cache, open addressing */
  while (ccap < 4 * q)
    ccap <<= 1;
  uint64_t *ck = calloc(ccap, 8);
  int64_t *cv = malloc((size_t)ccap * 8);
  char *obuf = malloc((size_t)q * 21 + 8), *ow = obuf;
  if (!ck || !cv || !obuf)
    return 2;
  for (uint32_t i = 0; i < q; i++) {
    uint64_t key = ((uint64_t)qs[i] << 32) | qt[i];
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) & (ccap - 1);
    while (ck[h] && ck[h] != key + 1)
      h = (h + 1) & (ccap - 1);
    int64_t ans = ck[h] ? cv[h] : (ck[h] = key + 1, cv[h] = solve(qs[i], qt[i]));
    if (ans < 0) {
      memcpy(ow, "-1\n", 3);
      ow += 3;
    } else {
      char tmp[20];
      int k = 0;
      uint64_t a = (uint64_t)ans;
      do {
        tmp[k++] = (char)('0' + a % 10);
        a /= 10;
      } while (a);
      while (k)
        *ow++ = tmp[--k];
      *ow++ = '\n';
    }
  }
  int out = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return out < 0 || write(out, obuf, (size_t)(ow - obuf)) < 0 ? 1 : 0;
}
