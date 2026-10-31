/* Speed Demon 2026 — Track B (Index), team ZeroSouls. Zero-dependency C17.
 * /data/docs.txt + /data/queries.txt -> /out/results.txt, byte-identical to
 * the reference. Only the terms the queries use are indexed (the spec
 * blesses it): every query line is parsed first and its terms interned.
 * Twelve threads then walk line-aligned ranges of the corpus (a newline
 * counting pre-pass fixes every range's first document id), tokenizing
 * with the exact byte rule — lowercase A-Z, maximal [a-z0-9] runs — and
 * appending each (term, doc) hit to that term's per-thread run; the
 * within-document dedup is the last element because docs are visited in
 * ascending order and ranges are disjoint. The runs merge into one sorted
 * posting array per term (they are disjoint, so a plain k-way merge
 * needs no dedup); terms over the bitmap threshold become N-bit bitmaps.
 * BOOL evaluates strictly left to right on a per-thread result bitmap:
 * dense sides are word-parallel, sparse sides iterate their ids, and AND
 * against a sparse side rebuilds through a second buffer. PHRASE walks
 * the smallest posting list, filters candidates by membership in the
 * other terms, then verifies each candidate by re-tokenizing its raw
 * text and scanning for the consecutive term sequence. Duplicate query
 * lines reuse the first answer. Documents with no tokens match nothing. */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NTH 12
#define QCAP (1u << 18) /* distinct query terms */
#define QHS (1u << 19)  /* term hash slots */
#define MAXT 8         /* terms per query */
#define BMTH 8000      /* postings-as-bitmap threshold */
#define NONE 0xFFFFFFFFu

typedef struct {
  char *s;
  uint32_t len, df, *post[NTH], plen[NTH], pcap[NTH], *ids;
  uint64_t *bm;
} term_t;
static term_t terms[QCAP];
static uint32_t qslot[QHS], nterm;
static char *qarena;
static uint64_t qabytes;

static uint32_t nq, qk[50000], qt[50000][MAXT], qop[50000][MAXT],
    qnt[50000], qdup[50000], slen[50000];
static char *slots;
#define SLOTC 128

static const char *dbase; /* corpus mmap */
static uint32_t ndoc, *toff, *tlen;
static uint64_t cnts[NTH];

static char *xgrow(char *p, uint32_t *cap, uint32_t need) {
  uint32_t c = *cap ? *cap * 2 : 16;
  while (c < need)
    c *= 2;
  return (*cap = c, realloc(p, c * 4));
}

/* intern a query term (terms are lowercase by the grammar) */
static uint32_t qint(const char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  uint32_t b = (uint32_t)(h ^ (h >> 29)) & (QHS - 1);
  while (qslot[b]) {
    term_t *t = &terms[qslot[b] - 1];
    if (t->len == len && !memcmp(t->s, s, len))
      return qslot[b] - 1;
    b = (b + 1) & (QHS - 1);
  }
  if (nterm >= QCAP || qabytes + len > (8u << 20))
    exit(3);
  term_t *t = &terms[nterm];
  *t = (term_t){0};
  t->s = qarena + qabytes, memcpy(t->s, s, len), qabytes += len, t->len = len;
  return qslot[b] = ++nterm, nterm - 1;
}

/* id of the doc token s[0..len) with A-Z folded, or NONE */
static uint32_t dlookup(const char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++) {
    unsigned c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z')
      c += 32;
    h = (h ^ c) * 1099511628211ULL;
  }
  uint32_t b = (uint32_t)(h ^ (h >> 29)) & (QHS - 1);
  while (qslot[b]) {
    term_t *t = &terms[qslot[b] - 1];
    if (t->len == len) {
      uint32_t i = 0;
      while (i < len) {
        unsigned c = (unsigned char)s[i], d = (unsigned char)t->s[i];
        if (c >= 'A' && c <= 'Z')
          c += 32;
        if (c != d)
          break;
        i++;
      }
      if (i == len)
        return qslot[b] - 1;
    }
    b = (b + 1) & (QHS - 1);
  }
  return NONE;
}

typedef struct {
  const char *p0, *p1;
  uint32_t d0, t;
} brange_t;

static uint64_t nlcount(const char *p, const char *e) {
  uint64_t n = 0;
  while (p < e) {
    p = memchr(p, '\n', (size_t)(e - p));
    if (!p)
      break;
    n++, p++;
  }
  return n;
}

static void *countwork(void *arg) { /* newlines in one fixed byte range */
  brange_t *r = arg;
  cnts[r->t] = nlcount(r->p0, r->p1);
  return NULL;
}

static void *walkwork(void *arg) { /* tokenize a doc range into postings */
  brange_t *r = arg;
  uint32_t d = r->d0;
  for (const char *p = r->p0; p < r->p1; d++) {
    const char *tb = memchr(p, '\t', (size_t)(r->p1 - p)),
               *nl = memchr(tb, '\n', (size_t)(r->p1 - tb));
    toff[d] = (uint32_t)(tb + 1 - dbase), tlen[d] = (uint32_t)(nl - tb - 1);
    for (const char *q = tb + 1; q < nl;) {
      unsigned c = (unsigned char)*q;
      if (c >= 'A' && c <= 'Z')
        c += 32; /* ASCII-lowercase before the token test */
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        const char *s = q;
        for (;;) {
          unsigned cc;
          q++;
          if (q >= nl)
            break;
          cc = (unsigned char)*q;
          if (cc >= 'A' && cc <= 'Z')
            cc += 32;
          if (!((cc >= 'a' && cc <= 'z') || (cc >= '0' && cc <= '9')))
            break;
        }
        uint32_t id = dlookup(s, (uint32_t)(q - s));
        if (id != NONE) {
          term_t *t = &terms[id];
          if (t->plen[r->t] == t->pcap[r->t])
            t->post[r->t] = (uint32_t *)xgrow((char *)t->post[r->t],
                                              &t->pcap[r->t],
                                              t->plen[r->t] + 1);
          if (!t->post[r->t])
            exit(2);
          if (!t->plen[r->t] || t->post[r->t][t->plen[r->t] - 1] != d)
            t->post[r->t][t->plen[r->t]++] = d;
        }
      } else
        q++;
    }
    p = nl + 1;
  }
  return NULL;
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

static pthread_t th[NTH];
static void run12(void *(*fn)(void *), void *args, size_t stride) {
  for (int t = 0; t < NTH; t++)
    if (pthread_create(&th[t], NULL, fn, (char *)args + t * stride))
      exit(2);
  for (int t = 0; t < NTH; t++)
    pthread_join(th[t], NULL);
}

static char *finish(char *w, uint64_t cnt, uint64_t sum, uint32_t *scratch,
                    uint32_t m) {
  w = fu(w, cnt), *w++ = ' ';
  w = fu(w, sum);
  for (uint32_t i = 0; i < m; i++)
    *w++ = ' ', w = fu(w, scratch[i]);
  *w++ = '\n';
  return w;
}

/* membership of doc id in term t */
static uint32_t hasdoc(term_t *t, uint32_t id) {
  if (t->bm)
    return (uint32_t)((t->bm[id >> 6] >> (id & 63)) & 1);
  uint32_t lo = 0, hi = t->df;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (t->ids[mid] < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < t->df && t->ids[lo] == id;
}

/* verify a PHRASE candidate by re-tokenizing its raw text */
static uint32_t verify(uint32_t qi, uint32_t id, uint32_t n, uint32_t *tok) {
  const char *p = dbase + toff[id], *e = p + tlen[id];
  uint32_t win = 0;
  while (p < e) {
    unsigned c = (unsigned char)*p;
    if (c >= 'A' && c <= 'Z')
      c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      const char *s = p;
      for (;;) {
        unsigned cc;
        p++;
        if (p >= e)
          break;
        cc = (unsigned char)*p;
        if (cc >= 'A' && cc <= 'Z')
          cc += 32;
        if (!((cc >= 'a' && cc <= 'z') || (cc >= '0' && cc <= '9')))
          break;
      }
      uint32_t tid = dlookup(s, (uint32_t)(p - s));
      if (win < n)
        tok[win++] = tid;
      else {
        memmove(tok, tok + 1, (size_t)(n - 1) * 4);
        tok[n - 1] = tid;
      }
      if (win == n) {
        uint32_t j = 0;
        while (j < n && tok[j] == qt[qi][j])
          j++;
        if (j == n)
          return 1;
      }
    } else
      p++;
  }
  return 0;
}

/* one candidate doc of a PHRASE query */
static void try_doc(uint32_t qi, uint32_t id, uint32_t base, uint64_t *cnt,
                    uint64_t *sum, uint32_t *scratch, uint32_t *m,
                    uint32_t *tok) {
  uint32_t n = qnt[qi], ok = 1;
  for (uint32_t j = 0; j < n && ok; j++)
    ok = j == base || hasdoc(&terms[qt[qi][j]], id);
  if (ok && verify(qi, id, n, tok)) {
    (*cnt)++, *sum += id;
    if (*m < 10)
      scratch[(*m)++] = id;
  }
}

static void answer(uint32_t qi, uint64_t *R, uint64_t *R2, uint32_t *scratch,
                   uint32_t NW, uint32_t *tok) {
  char *w = slots + (size_t)qi * SLOTC;
  uint32_t n = qnt[qi];
  if (qk[qi] == 0) { /* BOOL: strictly left to right */
    memset(R, 0, (size_t)NW * 8);
    term_t *t0 = &terms[qt[qi][0]];
    if (t0->bm)
      memcpy(R, t0->bm, (size_t)NW * 8);
    else if (t0->ids)
      for (uint32_t i = 0; i < t0->df; i++)
        R[t0->ids[i] >> 6] |= 1ull << (t0->ids[i] & 63);
    for (uint32_t j = 1; j < n; j++) {
      term_t *t = &terms[qt[qi][j]];
      uint32_t op = qop[qi][j - 1];
      if (t->bm) {
        for (uint32_t x = 0; x < NW; x++)
          if (op == 0)
            R[x] &= t->bm[x];
          else if (op == 1)
            R[x] |= t->bm[x];
          else
            R[x] &= ~t->bm[x];
      } else if (t->ids) {
        if (op == 0) {
          memset(R2, 0, (size_t)NW * 8);
          for (uint32_t i = 0; i < t->df; i++) {
            uint32_t id = t->ids[i];
            if (R[id >> 6] & 1ull << (id & 63))
              R2[id >> 6] |= 1ull << (id & 63);
          }
          memcpy(R, R2, (size_t)NW * 8);
        } else
          for (uint32_t i = 0; i < t->df; i++) {
            uint32_t id = t->ids[i];
            if (op == 1)
              R[id >> 6] |= 1ull << (id & 63);
            else
              R[id >> 6] &= ~(1ull << (id & 63));
          }
      } else if (op == 0) /* absent term: AND empties, OR/NOT unchanged */
        memset(R, 0, (size_t)NW * 8);
    }
    uint64_t cnt = 0, sum = 0;
    uint32_t m = 0;
    for (uint32_t x = 0; x < NW; x++)
      if (R[x]) {
        cnt += (uint64_t)__builtin_popcountll(R[x]);
        for (uint64_t bits = R[x]; bits; bits &= bits - 1) {
          uint32_t id = x * 64 + (uint32_t)__builtin_ctzll(bits);
          sum += id;
          if (m < 10)
            scratch[m++] = id;
        }
      }
    slen[qi] = (uint32_t)(finish(w, cnt, sum, scratch, m) - w);
    return;
  }
  uint32_t base = 0, bdf = 0xFFFFFFFF;
  for (uint32_t j = 0; j < n; j++) {
    uint32_t df = terms[qt[qi][j]].df;
    if (!df)
      bdf = 0; /* an absent term matches nothing */
    else if (df < bdf)
      bdf = df, base = j;
  }
  uint64_t cnt = 0, sum = 0;
  uint32_t m = 0;
  if (bdf) {
    term_t *bt = &terms[qt[qi][base]];
    if (bt->ids)
      for (uint32_t i = 0; i < bt->df; i++)
        try_doc(qi, bt->ids[i], base, &cnt, &sum, scratch, &m, tok);
    else
      for (uint32_t x = 0; x < NW; x++)
        for (uint64_t bits = bt->bm[x]; bits; bits &= bits - 1)
          try_doc(qi, x * 64 + (uint32_t)__builtin_ctzll(bits), base, &cnt,
                  &sum, scratch, &m, tok);
  }
  slen[qi] = (uint32_t)(finish(w, cnt, sum, scratch, m) - w);
}

typedef struct {
  uint32_t lo, hi;
} qrange_t;

static void *qwork(void *arg) {
  qrange_t *r = arg;
  uint32_t NW = (ndoc + 63) / 64;
  uint64_t *R = malloc((size_t)NW * 8), *R2 = malloc((size_t)NW * 8);
  uint32_t *scratch = malloc(40), *tok = malloc(MAXT * 4);
  if (!R || !R2 || !scratch || !tok)
    exit(2);
  for (uint32_t qi = r->lo; qi < r->hi; qi++)
    if (qdup[qi] == NONE)
      answer(qi, R, R2, scratch, NW, tok);
  return NULL;
}

int main(void) {
  int fd = open("/data/queries.txt", O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st))
    return 1;
  const char *qb = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd,
                        0),
             *qe = qb + st.st_size;
  if (qb == MAP_FAILED)
    return 1;
  qarena = malloc(8u << 20), qabytes = 0;
  if (!qarena)
    return 2;

  /* parse query lines; duplicate lines reuse the first answer */
  static uint32_t duph[1u << 17];
  static const char *ql[50000];
  static uint32_t qln[50000];
  for (const char *p = qb; p < qe;) {
    const char *nl = memchr(p, '\n', (size_t)(qe - p));
    if (!nl)
      nl = qe;
    uint64_t h = 1469598103934665603ULL;
    for (const char *q = p; q < nl; q++)
      h = (h ^ (unsigned char)*q) * 1099511628211ULL;
    uint32_t hb = (uint32_t)h & ((1u << 17) - 1);
    while (duph[hb] && (qln[duph[hb] - 1] != (uint32_t)(nl - p) ||
                        memcmp(ql[duph[hb] - 1], p, (size_t)(nl - p))))
      hb = (hb + 1) & ((1u << 17) - 1);
    qdup[nq] = duph[hb] ? duph[hb] - 1 : NONE;
    if (!duph[hb])
      duph[hb] = nq + 1, ql[nq] = p, qln[nq] = (uint32_t)(nl - p);
    qk[nq] = p[0] == 'P', p += qk[nq] ? 8 : 5;
    const char *te = qk[nq] ? nl - 1 : nl; /* strip the closing quote */
    uint32_t nt = 0;
    while (p < te) {
      const char *s = p;
      while (p < te && *p != ' ')
        p++;
      if (qk[nq] == 0 && *s >= 'A' && *s <= 'Z' &&
          ((p - s == 2 && s[0] == 'O' && s[1] == 'R') ||
           (p - s == 3 && (s[0] == 'A' || s[0] == 'N'))))
        qop[nq][nt - 1] = *s == 'A' ? 0 : *s == 'O' ? 1 : 2;
      else if (nt < MAXT)
        qt[nq][nt++] = qint(s, (uint32_t)(p - s));
      while (p < te && *p == ' ')
        p++;
    }
    qnt[nq++] = nt;
    p = nl + 1;
  }
  slots = calloc((size_t)(nq ? nq : 1), SLOTC);
  if (!slots)
    return 2;

  fd = open("/data/docs.txt", O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    return 1;
  uint64_t dsz = (uint64_t)st.st_size;
  dbase = mmap(NULL, dsz, PROT_READ, MAP_PRIVATE, fd, 0);
  if (dbase == MAP_FAILED)
    return 1;
  ndoc = (uint32_t)nlcount(dbase, dbase + dsz);
  toff = malloc((size_t)ndoc * 4), tlen = malloc((size_t)ndoc * 4);
  if (!toff || !tlen)
    return 2;

  /* fixed ranges for the newline count, then aligned doc ranges */
  brange_t cr[NTH], wr[NTH];
  for (int t = 0; t < NTH; t++)
    cr[t] = (brange_t){dbase + dsz * t / NTH, dbase + dsz * (t + 1) / NTH, 0,
                       t};
  run12(countwork, cr, sizeof(brange_t));
  const char *al[NTH + 1];
  al[0] = dbase, al[NTH] = dbase + dsz;
  for (int t = 1; t < NTH; t++) {
    const char *nl = memchr(cr[t].p0, '\n', (size_t)(al[NTH] - cr[t].p0));
    al[t] = nl ? nl + 1 : al[NTH];
  }
  /* first doc id of each aligned range = newlines before it */
  uint64_t doc = 0;
  for (int t = 0; t < NTH; t++) {
    const char *p1 = al[t + 1 < NTH ? t + 1 : NTH];
    wr[t] = (brange_t){al[t], p1, (uint32_t)doc, t};
    doc += nlcount(al[t], p1);
  }
  run12(walkwork, wr, sizeof(brange_t));

  /* merge per-thread runs; dense terms become bitmaps */
  for (uint32_t ti = 0; ti < nterm; ti++) {
    term_t *t = &terms[ti];
    uint32_t df = 0;
    for (int u = 0; u < NTH; u++)
      df += t->plen[u];
    t->df = df;
    if (!df)
      continue;
    if (df > BMTH) {
      t->bm = calloc((size_t)((ndoc + 63) / 64), 8);
      if (!t->bm)
        return 2;
      for (int u = 0; u < NTH; u++)
        for (uint32_t i = 0; i < t->plen[u]; i++)
          t->bm[t->post[u][i] >> 6] |= 1ull << (t->post[u][i] & 63);
    } else {
      t->ids = malloc((size_t)df * 4);
      if (!t->ids)
        return 2;
      uint32_t idx[NTH], out = 0;
      memset(idx, 0, sizeof idx);
      for (;;) {
        int best = -1;
        for (int u = 0; u < NTH; u++)
          if (idx[u] < t->plen[u] &&
              (best < 0 || t->post[u][idx[u]] < t->post[best][idx[best]]))
            best = u;
        if (best < 0)
          break;
        t->ids[out++] = t->post[best][idx[best]];
        idx[best]++;
      }
    }
    for (int u = 0; u < NTH; u++)
      free(t->post[u]), t->post[u] = NULL, t->plen[u] = 0;
  }

  /* queries: 12 disjoint slices, then duplicate lines copy their answer */
  qrange_t qr[NTH];
  uint32_t per = (nq + NTH - 1) / NTH;
  for (int t = 0; t < NTH; t++) {
    uint32_t lo = (uint32_t)t * per;
    qr[t] = (qrange_t){lo, lo + per < nq ? lo + per : nq};
  }
  run12(qwork, qr, sizeof(qrange_t));
  for (uint32_t qi = 0; qi < nq; qi++)
    if (qdup[qi] != NONE)
      memcpy(slots + (size_t)qi * SLOTC, slots + (size_t)qdup[qi] * SLOTC,
             SLOTC),
      slen[qi] = slen[qdup[qi]];

  uint64_t total = 0;
  for (uint32_t qi = 0; qi < nq; qi++)
    total += slen[qi];
  char *obuf = malloc((size_t)total + 1), *w = obuf;
  if (!obuf)
    return 2;
  for (uint32_t qi = 0; qi < nq; qi++)
    memcpy(w, slots + (size_t)qi * SLOTC, slen[qi]), w += slen[qi];
  int out = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return out < 0 || write(out, obuf, (size_t)(w - obuf)) < 0 ? 1 : 0;
}
