/* Speed Demon 2026 — Track C (Join), team ZeroSouls. Zero-dependency C17.
 * Reads /data/records.csv + /data/regions.csv, writes /out/aggregates.txt
 * byte-identical to the reference. One mmap, one byte-level pass of a CSV
 * state machine: BOMs are skipped at record starts only, quoted fields are
 * unescaped in place ("" -> ", LF inside quotes is data, trailing spaces
 * count), numerics parse as exact integers, no floats anywhere. The join is
 * a direct-index region table; aggregates are per-country counters, and
 * names_distinct is exact: names are interned (FNV-1a, open addressing) and
 * each country keeps a bitmap of interned ids. The board pre-warms /data, so
 * the mapping costs ~0 in memory.peak: peak memory is our heap alone.
 * Fixed capacities cover every scale the generator can emit (spec 2.2,
 * holdout included) with headroom; anything beyond them exits 3. */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXC 512 /* countries (spec: 240) plus _UNMATCHED */
#define RTAB (1u << 24) /* region table; ids stay under 10^7 (2.2) */
#define NSLOT (1u << 19) /* name hash slots; <=2^18 names, load <= 50% */

static uint32_t nc; /* interned country codes */
static char ccodes[MAXC][4];
static uint32_t code_slot[1024];
static uint8_t *rtab;
static uint64_t g_cnt[MAXC], g_ids[MAXC], g_nb[MAXC], g_ob[MAXC], g_dist[MAXC];
static int64_t g_sum[MAXC], g_min[MAXC], g_max[MAXC];
static uint64_t *g_bits[MAXC]; /* per-country seen-name bitmap */
static uint32_t g_bcap[MAXC];
static char *scr; /* scratch for one unescaped field */
static uint32_t scr_cap, h_names, h_bytes;
typedef struct {
  uint64_t h;
  uint32_t off, len;
} nent;            /* one interned name */
static nent *nents; /* by name id */
static uint32_t *h_slot; /* name id + 1 per slot, 0 = empty */
static char *h_arena;

static char *xgrow(char *p, uint32_t *cap, uint32_t need) {
  uint32_t c = *cap ? *cap * 2 : 4096;
  while (c < need)
    c *= 2;
  *cap = c;
  return realloc(p, c);
}

/* id of the exact byte string s[0..len): FNV-1a, linear probing */
static uint32_t intern(const unsigned char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h ^= s[i], h *= 1099511628211ULL;
  for (uint32_t b = (uint32_t)((h ^ (h >> 32)) & (NSLOT - 1));;
       b = (b + 1) & (NSLOT - 1)) {
    uint32_t id = h_slot[b];
    if (id) {
      if (nents[id - 1].h == h && nents[id - 1].len == len &&
          !memcmp(h_arena + nents[id - 1].off, s, len))
        return id - 1;
      continue;
    }
    if (h_names >= NSLOT / 2 || h_bytes + len > (8u << 20))
      exit(3); /* beyond every scale the generator emits */
    nents[h_names] = (nent){h, h_bytes, len};
    memcpy(h_arena + h_bytes, s, len);
    h_bytes += len;
    return (h_slot[b] = ++h_names) - 1;
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
  /* regions.csv: region_id -> country slot (1-based, 0 = absent) */
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
  const uint32_t UNM = nc; /* slot of _UNMATCHED */

  fd = open("/data/records.csv", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    return 1;
  uint64_t size = (uint64_t)st.st_size;
  const unsigned char *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (base == MAP_FAILED)
    return 1;
  madvise((void *)base, size, MADV_SEQUENTIAL);
  const unsigned char *p = base, *end = base + size;
  if (size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
    p += 3;
  static const char HDR[] = "id,name,note,region_id,amount";
  if ((size_t)(end - p) < sizeof HDR - 1 || memcmp(p, HDR, sizeof HDR - 1) ||
      !(p = memchr(p, '\n', end - p)))
    exit(3); /* one plain header line */
  p++;
  h_slot = calloc(NSLOT, sizeof *h_slot);
  nents = malloc((size_t)(NSLOT / 2) * sizeof *nents);
  h_arena = malloc(8u << 20);
  for (uint32_t i = 0; i <= nc; i++)
    g_min[i] = INT64_MAX, g_max[i] = INT64_MIN;

  while (p < end) {
    if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
      p += 3; /* a BOM is only legal at a record start */
    uint64_t id = 0, reg = 0, cents = 0;
    while (*p != ',')
      id = id * 10 + (*p++ - '0');
    p++;
    const unsigned char *nm;
    uint32_t nml;
    if (*p == '"') { /* name: quoted (the usual case) or empty unquoted */
      p = qfield(p, 1, &nml);
      nm = (const unsigned char *)scr;
    } else {
      for (nm = p; *p != ',';)
        p++;
      nml = (uint32_t)(p - nm);
      p++;
    }
    uint32_t ntl; /* note: only its unescaped length matters */
    if (*p == '"')
      p = qfield(p, 0, &ntl);
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
    if (p += 3, p < end) { /* CRLF or LF; the file always ends with one */
      if (*p == '\r')
        p += 2;
      else if (*p == '\n')
        p++;
      else
        exit(3);
    }
    uint32_t c = reg < RTAB && rtab[reg] ? rtab[reg] - 1 : UNM;
    int64_t amt = neg ? -(int64_t)cents : (int64_t)cents;
    g_cnt[c]++, g_ids[c] += id, g_sum[c] += amt, g_nb[c] += nml,
        g_ob[c] += ntl;
    if (amt < g_min[c])
      g_min[c] = amt;
    if (amt > g_max[c])
      g_max[c] = amt;
    uint32_t nid = intern(nm, nml), wi = nid >> 6;
    if (wi >= g_bcap[c]) { /* grow this country's name bitmap */
      uint32_t cap = g_bcap[c] ? g_bcap[c] * 2 : 16;
      while (cap <= wi)
        cap *= 2;
      g_bits[c] = realloc(g_bits[c], (size_t)cap * 8);
      memset(g_bits[c] + g_bcap[c], 0, (size_t)(cap - g_bcap[c]) * 8);
      g_bcap[c] = cap;
    }
    if (!(g_bits[c][wi] & 1ull << (nid & 63)))
      g_bits[c][wi] |= 1ull << (nid & 63), g_dist[c]++;
  }

  uint32_t order[MAXC], n = 0;
  for (uint32_t i = 0; i <= nc; i++)
    order[n++] = i;
  qsort(order, n, sizeof *order, keycmp);
  char *obuf = malloc(64 << 10), *w = obuf; /* full output is ~17 KB */
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
  if (out < 0)
    return 1;
  return write(out, obuf, (size_t)(w - obuf)) < 0 ? 1 : 0;
}
