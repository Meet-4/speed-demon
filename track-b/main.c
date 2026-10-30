/* Speed Demon 2026 — Track B (Index), team ZeroSouls. Zero-dependency C17.
 * Reads /data/docs.txt + /data/queries.txt, writes /out/results.txt
 * byte-identical to the reference.
 *
 * The queries ship with the corpus, so they are parsed first and only their
 * terms are interned; a 2^20-bit filter then lets the corpus pass skip every
 * token no query uses. One byte-level pass over the mapped corpus appends
 * each document id to the postings of the query terms the document contains
 * (ids are dense and visited in order, so postings come out deduplicated and
 * ascending). BOOL folds the operator chain strictly left to right over
 * sorted-id arrays: merge intersection, merge union, merge difference, no
 * precedence. PHRASE intersects the term postings smallest list first, then
 * re-tokenizes each candidate document straight from the mapping and scans
 * for the exact consecutive token run, so no positions are stored. Every
 * answer is "count sum id1..id10" formatted into one growable buffer. */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NSLOT (1u << 20) /* term table slots, power of two */
#define MAXT (1u << 19)  /* distinct query terms, load <= 50% */
#define ARENA (1u << 24) /* term bytes: MAXT terms of <= 32 bytes */
#define ALNUM(c) ((((c) | 32) >= 'a' && ((c) | 32) <= 'z') || ((c) >= '0' && (c) <= '9'))

typedef struct {
  uint64_t h;
  uint32_t off, len, df, cap;
  uint32_t *post; /* matching document ids, ascending */
} term;

static term terms[MAXT];
static uint32_t slot[NSLOT], nterms, abytes;
static uint64_t bloom[1 << 14]; /* presence bits: skip tokens no query uses */
static unsigned char *docs, *queries;
static uint64_t dsz, qsz, *doff; /* doff[d] = offset of doc d's text */
static uint32_t ndocs, dcap, *rbuf[2];
static char *obuf;
static uint64_t olen;
static uint32_t ocap;

static uint64_t fnv(const unsigned char *s, uint32_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < len; i++)
    h = (h ^ s[i]) * 1099511628211ULL;
  return h;
}

/* id of the exact byte string s (a query term); inserts it when new */
static uint32_t intern(const unsigned char *s, uint32_t len) {
  uint64_t h = fnv(s, len);
  for (uint32_t b = (uint32_t)(h ^ (h >> 32)) & (NSLOT - 1);; b = (b + 1) & (NSLOT - 1)) {
    uint32_t id = slot[b];
    if (!id) {
      if (nterms >= MAXT || abytes + len > ARENA)
        exit(3); /* beyond every scale the grammar can emit */
      term *t = &terms[nterms];
      t->h = h, t->off = abytes, t->len = len;
      memcpy(arena_ptr(), s, len); /* placeholder, patched below */
      abytes += len;
      bloom[(h >> 26) & 0x3FFF] |= 1ull << ((h >> 20) & 63);
      slot[b] = ++nterms;
      return nterms - 1;
    }
    term *t = &terms[id - 1];
    if (t->h == h && t->len == len && !memcmp(arena_ptr() + t->off, s, len))
      return id - 1;
  }
}
