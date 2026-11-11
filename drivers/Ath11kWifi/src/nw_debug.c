/* The ath11k driver's message functions, printing through the module log (ath11k's own debug.c prints with %pV, which the log cannot). */
#include "lx_pci.h"
#include "lx_plat.h"
#include "core.h"
#include "debug.h"

static void emit(int level, const char *fmt, va_list ap) {
  char b[300]; vsnprintf(b, sizeof b, fmt, ap); size_t n = strlen(b); if (n && b[n - 1] == '\n') b[n - 1] = 0;
  lx_plat->log(level, b); if (lx_log_tap) lx_log_tap(level, b); }
void ath11k_info(struct ath11k_base *ab, const char *fmt, ...) { (void)ab; va_list ap; va_start(ap, fmt); emit(6, fmt, ap); va_end(ap); }
void ath11k_err(struct ath11k_base *ab, const char *fmt, ...) { (void)ab; va_list ap; va_start(ap, fmt); emit(3, fmt, ap); va_end(ap); }
void ath11k_warn(struct ath11k_base *ab, const char *fmt, ...) { (void)ab; va_list ap; va_start(ap, fmt); emit(4, fmt, ap); va_end(ap); }
void __ath11k_dbg(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *fmt, ...) {
  (void)ab; if (!(ath11k_debug_mask & mask)) return; va_list ap; va_start(ap, fmt); emit(7, fmt, ap); va_end(ap); }
void ath11k_dbg_dump(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *msg, const char *prefix, const void *buf, size_t len) {
  (void)ab; (void)prefix; if (!(ath11k_debug_mask & mask)) return; if (msg) lx_plat->log(7, msg);
  const u8 *p = buf; char l[100];
  for (size_t i = 0; i < len; i += 16) { int o = snprintf(l, sizeof l, "%04zx:", i); for (size_t k = i; k < i + 16 && k < len; k++) o += snprintf(l + o, sizeof l - o, " %02x", p[k]); lx_plat->log(7, l); } }
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii) {
  (void)groupsize; (void)ascii; const u8 *p = buf; int o = 0;
  if (linebuflen) linebuf[0] = 0;
  for (size_t i = 0; i < len && i < (size_t)rowsize && o + 4 < (int)linebuflen; i++) o += snprintf(linebuf + o, linebuflen - o, "%s%02x", i ? " " : "", p[i]);
  return o; }

/* The firmware's own log (WMI_DIAG_EVENTID; the host asks for it with the QMI ini request's enablefwlog, as ath11k does for the WCN6750).
 * An event's bytes: the WMI TLV header, a word (offset), then records as qca-wifi-host-cmn utils/fwlog/dbglog_host.h reads them:
 * word 0 type << 24 | timestamp, word 1 message id << 16 | vdev << 11 | level << 8 | payload bytes, the payload (32-bit arguments).
 * This firmware also sends type 7 records with one more header word: type << 24, a time word, then the id and length word.
 * The message ids index the firmware image's Data.msc dictionary (format strings with file and line), decoded off the device. The last
 * records are kept; the board logs them when the WPSS dies. */
#define NW_FWD_RECORDS 1024
#define NW_FWD_ARGS 6
struct nw_fwd { u32 w0, w1, a[NW_FWD_ARGS]; };
static struct nw_fwd *nw_fwd;
static unsigned nw_fwd_total, nw_fwd_events, nw_fwd_bad, nw_fwd_raw;
void nw_fw_diag(const void *data, unsigned long len) {
  const u8 *p = data;
  nw_fwd_events++;
  if (!nw_fwd) nw_fwd = kzalloc(NW_FWD_RECORDS * sizeof(*nw_fwd), GFP_ATOMIC);
  if (!nw_fwd) return;
  if (nw_fwd_raw < 2 && len >= 40) {  /* the first events' start as they came, to check the layout above */
    u32 w[10]; memcpy(w, p, sizeof(w)); nw_fwd_raw++;
    lx_log(6, "fw diag raw %lu: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", len, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
  }
  if (len < 8) return;
  p += 8; len -= 8;
  while (len >= 8) {
    struct nw_fwd *r = &nw_fwd[nw_fwd_total % NW_FWD_RECORDS];
    u32 w[2]; memcpy(w, p, 8);
    unsigned long head = 8;
    if ((w[0] >> 24) == 7) {  /* kept as type 7 << 24 | the time word's low 24 bits, then the id and length word */
      if (len < 12) { nw_fwd_bad++; break; }
      u32 w2; memcpy(&w2, p + 8, 4);
      w[0] = 7u << 24 | (w[1] & 0xffffff); w[1] = w2; head = 12;
    }
    const unsigned long pay = (w[1] & 0xff), padded = (pay + 3) & ~3ul;
    if (padded > len - head) { nw_fwd_bad++; break; }
    r->w0 = w[0]; r->w1 = w[1]; memset(r->a, 0, sizeof(r->a));
    memcpy(r->a, p + head, pay < sizeof(r->a) ? pay : sizeof(r->a));
    nw_fwd_total++;
    p += head + padded; len -= head + padded;
  }
}
/* Logs count records ending skip records before the newest, oldest first: "fwd <timestamp> <id>/<payload bytes>: args". */
void nw_fw_diag_dump(unsigned skip, unsigned count) {
  const unsigned have = nw_fwd_total < NW_FWD_RECORDS ? nw_fwd_total : NW_FWD_RECORDS;
  lx_log(6, "fw diag: %u events, %u records, %u bad; dumping %u ending %u back", nw_fwd_events, nw_fwd_total, nw_fwd_bad, count, skip);
  if (!nw_fwd || skip >= have) return;
  if (count > have - skip) count = have - skip;
  for (unsigned i = nw_fwd_total - skip - count; i != nw_fwd_total - skip; i++) {
    const struct nw_fwd *r = &nw_fwd[i % NW_FWD_RECORDS]; const unsigned pay = r->w1 & 0xff;
    lx_log(6, "fwd %06x %u/%u/%u: %x %x %x %x %x %x", r->w0 & 0xffffff, r->w0 >> 24, r->w1 >> 16, pay, r->a[0], r->a[1], r->a[2], r->a[3], r->a[4], r->a[5]);
  }
}
