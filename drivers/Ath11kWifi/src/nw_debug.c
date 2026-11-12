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
#define NW_WMI_CMDS 1024
struct nw_wmi { u32 id, len, p[NW_FWD_ARGS]; u8 has_p; };
static struct nw_wmi *nw_wmi;
static unsigned nw_wmi_total;
static void nw_wmi_add(u32 id, u32 len) {
  if (!nw_wmi) nw_wmi = kzalloc(NW_WMI_CMDS * sizeof(*nw_wmi), GFP_ATOMIC);
  if (!nw_wmi) return;
  struct nw_wmi *w = &nw_wmi[nw_wmi_total++ % NW_WMI_CMDS];
  memset(w, 0, sizeof(*w)); w->id = id; w->len = len;
}
static void nw_wmi_params(const u32 *a) {
  if (!nw_wmi || !nw_wmi_total) return;
  struct nw_wmi *w = &nw_wmi[(nw_wmi_total - 1) % NW_WMI_CMDS];
  if (w->has_p) return;
  memcpy(w->p, a, sizeof(w->p)); w->has_p = 1;
}
/* Logs the kept WMI commands from index from, lines a call: "wmi N: cmd len | first words"; returns the next index, 0 when done. */
int nw_fw_wmi_dump(int from, int lines) {
  const unsigned first = nw_wmi_total > NW_WMI_CMDS ? nw_wmi_total - NW_WMI_CMDS : 0;
  if (!nw_wmi) { lx_log(6, "wmi stream: none kept"); return 0; }
  if ((unsigned)from < first) { lx_log(6, "wmi stream: %u commands, %u kept", nw_wmi_total, nw_wmi_total - first); from = first; }
  for (int n = 0; n < lines && (unsigned)from < nw_wmi_total; n++, from++) {
    const struct nw_wmi *w = &nw_wmi[from % NW_WMI_CMDS];
    lx_log(6, "wmi %d: %x %u | %x %x %x %x %x %x", from, w->id, w->len, w->p[0], w->p[1], w->p[2], w->p[3], w->p[4], w->p[5]);
  }
  return (unsigned)from < nw_wmi_total ? from : 0;
}
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
    { /* the WMI commands as the firmware received them (wmi_svc.c 11439 "WMI_CMD_RX CmdId Len", then 11440-11447 "WMI_CMD_PARAMS"):
       * kept in their own ring for nw_fw_wmi_dump, to diff with the vendor host's stream */
      const unsigned wid = r->w1 >> 16;
      if (wid == 11439) nw_wmi_add(r->a[0], r->a[1]);
      else if (wid >= 11440 && wid <= 11447) nw_wmi_params(r->a);
    }
    { /* the firmware's records of the host's receive setup (htt_tgt_rx.c 1261-1269, wal_rx_control.c 1103-1116), logged as they come */
      static unsigned setup_logged; const unsigned id = r->w1 >> 16;
      if (((id >= 1261 && id <= 1269) || (id >= 1103 && id <= 1116)) && setup_logged < 60) {
        setup_logged++;
        lx_log(6, "fwd %06x %u/%u/%u: %x %x %x %x %x %x", r->w0 & 0xffffff, r->w0 >> 24, id, (unsigned)pay, r->a[0], r->a[1], r->a[2],
               r->a[3], r->a[4], r->a[5]);
      }
    }
    p += head + padded; len -= head + padded;
  }
}
/* Logs every kept record whose message id lies in [lo, hi] (Data.msc), oldest first, at most max of them: the firmware's own account of
 * the host's ring setup (htt_tgt_rx.c 1261-1269, wal_rx_control.c 1103-1116) to compare with the vendor host's (worker only). */
void nw_fw_diag_dump_ids(unsigned lo, unsigned hi, unsigned max) {
  const unsigned have = nw_fwd_total < NW_FWD_RECORDS ? nw_fwd_total : NW_FWD_RECORDS;
  unsigned shown = 0;
  if (!nw_fwd) return;
  for (unsigned i = nw_fwd_total - have; i != nw_fwd_total && shown < max; i++) {
    const struct nw_fwd *r = &nw_fwd[i % NW_FWD_RECORDS]; const unsigned id = r->w1 >> 16, pay = r->w1 & 0xff;
    if (id < lo || id > hi) continue;
    shown++;
    lx_log(6, "fwd %06x %u/%u/%u: %x %x %x %x %x %x", r->w0 & 0xffffff, r->w0 >> 24, id, pay, r->a[0], r->a[1], r->a[2], r->a[3], r->a[4], r->a[5]);
  }
  lx_log(6, "fw diag ids %u-%u: %u of %u kept records", lo, hi, shown, have);
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
