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
