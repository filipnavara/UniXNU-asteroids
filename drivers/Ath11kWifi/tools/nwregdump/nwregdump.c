// SPDX-License-Identifier: GPL-2.0
/*
 * nwregdump: reads the WCN6750 data-path registers (scripts/gen-wcn-reglist.py, src/nw_wcn_regs.h) while Qualcomm's own WLAN stack
 * (icnss2 + qca_cld3) runs on the Nothing Phone (3a), and prints them to the kernel log for scripts/diff-wcn-regs.py; the same table
 * as wifi-regdump in our port. Every load takes one dump, then the init returns -ENODEV so the module unloads itself.
 *
 * Built without a kernel tree (tools/nwregdump/build.sh): the GKI 6.1 struct module layout and the imported symbols' CRCs are
 * copied from the phone's own modules (icnss2.ko), the init carries the kernel's kCFI type hash for int (void) and a BTI landing pad.
 * The register window is the chip's (QMI device info BAR 0x61e00000); WCN6750 reaches UMAC through static window 1 (+0x80000) and the
 * copy engines through window 2 (+0x100000), 512 KiB each, as ath11k's ath11k_ahb_window_read32_wcn6750 and the vendor's HAL do.
 */
typedef unsigned int u32;
typedef unsigned long u64;
struct device;
struct device_node;

extern int _printk(const char *fmt, ...);
extern void msleep(unsigned int ms);
extern struct device_node *of_find_node_by_name(struct device_node *from, const char *name);
extern void *of_find_device_by_node(struct device_node *np);
extern void put_device(struct device *dev);
extern void *devm_ioremap(struct device *dev, u64 offset, u64 size);
extern int icnss_force_wake_request(struct device *dev);
extern int icnss_force_wake_release(struct device *dev);
extern int icnss_is_device_awake(struct device *dev);
extern volatile unsigned long jiffies;

#include "../../src/nw_wcn_regs.h"

#define BAR 0x61e00000ul
/* NW_BLOCK picks the block a build dumps (one per module, so a hang points at a block): 0 WBM, 1 REO, 2 TCL, 3 copy engines,
 * -1 all. Registers outside it print as xxxxxxxx. */
#ifndef NW_BLOCK
#define NW_BLOCK -1
#endif
#ifndef NW_NAME
#define NW_NAME "nwregdump"
#endif
static int in_block(u32 off)
{
	switch (NW_BLOCK) {
	case 0: return off >= 0xa34000 && off < 0xa38000;
	case 1: return off >= 0xa38000 && off < 0xa3c000;
	case 2: return off >= 0xa44000 && off < 0xa48000;
	case 3: return off >= 0x1b80000;
	default: return 1;
	}
}
#define WINDOW 0x80000u

static u32 read_reg(volatile unsigned char *win, u32 off)
{
	u32 at = ((off ^ 0xa00000u) < WINDOW) ? 0 : WINDOW;  /* window 1 (UMAC) at +0, window 2 (CE) at +WINDOW in our mapping */
	u32 v;

	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(win + at + (off & (WINDOW - 1))) : "memory");
	return v;
}

__attribute__((section(".init.text"), used)) int init_module(void)
{
	struct device_node *np = of_find_node_by_name((struct device_node *)0, "qcom,wcn6750");
	unsigned char *pdev;
	struct device *dev;
	volatile unsigned char *win;
	u32 vals[8];
	int i, awake = 0;
	unsigned long stamp = jiffies;

	if (!np) {
		_printk("\0013nwreg: no qcom,wcn6750 node\n");
		return -19;
	}
	pdev = of_find_device_by_node(np);  /* the node reference is kept: of_node_put is not exported to modules by this GKI */
	if (!pdev) {
		_printk("\0013nwreg: no wcn6750 device\n");
		return -19;
	}
	dev = (struct device *)(pdev + 16);  /* struct platform_device: name, id, id_auto, then the struct device */
	i = icnss_force_wake_request(dev);
	for (int t = 0; t < 50 && !(awake = icnss_is_device_awake(dev)); t++)
		msleep(2);
	_printk("\0016nwreg: begin %lu: block %d, wake request %d, awake %d, %d registers\n", stamp, NW_BLOCK, i, awake, NW_WCN_REGS);
	if (awake) {
		win = devm_ioremap(dev, BAR + WINDOW, 2 * WINDOW);
		if (!win) {
			_printk("\0013nwreg: no mapping\n");
		} else {
			for (i = 0; i < NW_WCN_REGS; i += 8) {
				int n = NW_WCN_REGS - i < 8 ? NW_WCN_REGS - i : 8, any = 0;
				char text[8][9];

				for (int k = 0; k < 8; k++) {
					if (k < n && in_block(nw_wcn_regs[i + k])) {
						vals[k] = read_reg(win, nw_wcn_regs[i + k]);
						for (int d = 0; d < 8; d++)
							text[k][d] = "0123456789abcdef"[(vals[k] >> (28 - 4 * d)) & 15];
						any = 1;
					} else {
						for (int d = 0; d < 8; d++)
							text[k][d] = 'x';
					}
					text[k][8] = 0;
				}
				if (any)
					_printk("\0016nwreg %lu %d: %s %s %s %s %s %s %s %s\n", stamp, i, text[0], text[1], text[2], text[3],
						text[4], text[5], text[6], text[7]);
			}
		}
	}
	icnss_force_wake_release(dev);
	put_device(dev);
	_printk("\0016nwreg: end %lu\n", stamp);
	return -19;  /* one dump per load: unload again */
}

/* The arm64 loader sizes these from the relocations (module PLTs, ftrace trampoline); kbuild's module.lds normally adds them. */
__asm__(".section .plt,\"ax\",@nobits\n.p2align 3\n"
	".section .init.plt,\"ax\",@nobits\n.p2align 3\n"
	".section .text.ftrace_trampoline,\"ax\",@nobits\n.p2align 3\n"
	".text\n");

/* struct module of the phone's GKI 6.1 build: 0x440 bytes, name at 0x18 (MODULE_NAME_LEN 56), init at 0x170 (from icnss2.ko). */
struct this_module_layout {
	char pad0[0x18];
	char name[56];
	char pad1[0x170 - 0x18 - 56];
	int (*init)(void);
	char pad2[0x440 - 0x170 - 8];
};
__attribute__((section(".gnu.linkonce.this_module"), aligned(64), used))
struct this_module_layout __this_module = {.name = NW_NAME, .init = init_module};

__attribute__((section(".modinfo"), used, aligned(1))) static const char modinfo[] =
	"license=GPL\0"
	"vermagic=6.1.172-android14-11-maybe-dirty SMP preempt mod_unload modversions aarch64\0"
	"name=" NW_NAME "\0"
	"depends=icnss2\0";

struct modversion { u64 crc; char name[56]; };
/* CRCs from the phone's modules' __versions (icnss2.ko, qca_cld3_qca6750.ko); symbols missing here only draw a warning. */
__attribute__((section("__versions"), used, aligned(8))) static const struct modversion versions[] = {
	{0xea759d7f, "module_layout"},
	{0x92997ed8, "_printk"},
	{0xf9a482f9, "msleep"},
	{0xfb2db23b, "devm_ioremap"},
	{0xc473e973, "of_find_node_by_name"},
	{0xa81f2f21, "icnss_force_wake_request"},
	{0xeefa4468, "icnss_force_wake_release"},
	{0x36ffff61, "icnss_is_device_awake"},
};
