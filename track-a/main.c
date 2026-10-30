/* Speed Demon 2026 — Track A (Ingest), team ZeroSouls. Zero-dependency C17.
 * /data/requests.log -> /out/summary.txt, byte-identical to the reference.
 * mmap the input; two passes (count groups, then fill a dense latency arena);
 * nearest-rank percentiles per group; hours fixed-order, endpoints interned
 * and sorted bytewise (prefix first). */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOURS 72
#define MAXEP 4096 /* distinct endpoints (catalogue is 1,842) */
#define EPS 8192   /* interning slots, power of two */
#define KEY(h, e) (((uint64_t)(h) * MAXEP) + (e))

static char *ep_arena;
static uint32_t ep_len[MAXEP], ep_off[MAXEP + 1], ep_slot[EPS];
static uint64_t ep_hash[MAXEP];
static int nep;
static uint32_t g_count[HOURS * MAXEP], g_errs[HOURS * MAXEP];
static uint64_t g_pos[HOURS * MAXEP], g_fill[HOURS * MAXEP];
static uint32_t *g_lat;

static uint32_t intern(const char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  uint32_t b = (uint32_t)(h ^ (h >> 29)) & (EPS - 1);
  while (ep_slot[b]) {
    uint32_t id = ep_slot[b] - 1;
    if (ep_hash[id] == h && ep_len[id] == len &&
        !memcmp(ep_arena + ep_off[id], s, len))
      return id;
    b = (b + 1) & (EPS - 1);
  }
  uint32_t id = (uint32_t)nep++;
  if (nep > MAXEP || ep_off[id] + len > (1u << 20))
    exit(3);
  memcpy(ep_arena + ep_off[id], s, len);
  ep_len[id] = len, ep_hash[id] = h, ep_off[id + 1] = ep_off[id] + len,
  ep_slot[b] = id + 1;
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

static int u32cmp(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static int epcmp(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  uint32_t m = ep_len[x] < ep_len[y] ? ep_len[x] : ep_len[y];
  int c = memcmp(ep_arena + ep_off[x], ep_arena + ep_off[y], m);
  return c ? c : (ep_len[x] < ep_len[y] ? -1 : ep_len[x] > ep_len[y]);
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
  const char *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0), *p, *end;
  if (base == MAP_FAILED)
    return 1;
  ep_arena = malloc(1 << 20);
  ep_off[0] = 0;
  uint32_t h, e, er, la;
  for (p = base, end = base + size; p < end;) /* pass 1: counts and errors */
    p = parse(p, &h, &e, &er, &la), g_count[KEY(h, e)]++,
    g_errs[KEY(h, e)] += er;
  uint64_t tot = 0;
  for (uint64_t i = 0; i < (uint64_t)HOURS * MAXEP; i++)
    g_pos[i] = g_fill[i] = tot, tot += g_count[i];
  g_lat = malloc(tot ? tot * 4 : 4);
  for (p = base; p < end;) /* pass 2: latencies into the dense arena */
    p = parse(p, &h, &e, &er, &la), g_lat[g_fill[KEY(h, e)]++] = la;
  munmap((void *)base, size);
  uint32_t *order = malloc((size_t)nep * 4);
  for (int i = 0; i < nep; i++)
    order[i] = (uint32_t)i;
  qsort(order, (size_t)nep, 4, epcmp);
  char hs[HOURS][24];
  for (int i = 0, mo = 10, dy = 30, hh = 18; i < HOURS; i++) {
    snprintf(hs[i], 24, "2026-%02d-%02dT%02d:00:00Z", mo, dy, hh);
    if (++hh == 24) {
      hh = 0, ++dy;
      if (mo == 10 && dy == 32) /* Oct 31 23:00 is the last hour of Oct */
        dy = 1, mo = 11;
    }
  }
  char *obuf = malloc(16 << 20); /* full output is ~10.1 MB */
  uint64_t ow = 0;
  for (int h2 = 0; h2 < HOURS; h2++)
    for (int k = 0; k < nep; k++) {
      uint32_t id = order[k], n = g_count[KEY(h2, id)];
      if (!n)
        continue;
      uint32_t *v = g_lat + g_pos[KEY(h2, id)];
      qsort(v, n, 4, u32cmp);
      /* nearest-rank: 1-indexed rank (p*n+99)/100, so 0-indexed minus one */
      ow += (uint64_t)sprintf(obuf + ow, "%s %.*s %u %u %u %u %u %u\n",
                             hs[h2], (int)ep_len[id], ep_arena + ep_off[id], n,
                             v[(50ULL * n + 99) / 100 - 1],
                             v[(95ULL * n + 99) / 100 - 1],
                             v[(99ULL * n + 99) / 100 - 1],
                             g_errs[KEY(h2, id)],
                             (uint32_t)(g_errs[KEY(h2, id)] * 1000000ULL / n));
    }
  return write(out, obuf, ow) < 0 ? 1 : 0;
}
