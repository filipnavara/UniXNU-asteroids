/* Kernel facilities the ath11k sources (AHB bus, WCN6750) use beyond those the ath12k sources need. */
#ifndef LX_ATH11K_H
#define LX_ATH11K_H
#include "lx_ath.h"
struct ath11k_base; struct ath11k;
typedef unsigned long kernel_ulong_t;
/* DMI: no firmware tables on this board */
enum dmi_field { DMI_NONE, DMI_BIOS_VENDOR, DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_BOARD_VENDOR, DMI_BOARD_NAME, DMI_STRING_MAX };
struct dmi_strmatch { unsigned char slot:7; unsigned char exact_match:1; char substr[79]; };
struct dmi_system_id { int (*callback)(const struct dmi_system_id *); const char *ident; struct dmi_strmatch matches[4]; void *driver_data; };
#define DMI_MATCH(a, b) { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }
#define dmi_first_match(l) ((const struct dmi_system_id *)0)
/* power management notifications: none */
#define PM_HIBERNATION_PREPARE 1
#define PM_POST_HIBERNATION 2
#define PM_SUSPEND_PREPARE 3
#define PM_POST_SUSPEND 4
#define PM_RESTORE_PREPARE 5
#define PM_POST_RESTORE 6
#define unregister_pm_notifier(n) 0
#define of_machine_is_compatible(c) 0
/* DMA: the board's window is one kind of memory */
#define dma_alloc_noncoherent(d, n, h, dir, f) lx_dma_alloc(d, n, h)
#define dma_free_noncoherent(d, n, p, h, dir) lx_dma_free(d, n, p, h)
#define prefetch(p) ((void)(p))
static inline void memset32(u32 *p, u32 v, size_t n) { while (n--) *p++ = v; }
#define device_get_mac_address(d, a) (-ENOENT)
typedef u32 phandle;
#define resource_size(r) ((r)->end - (r)->start + 1)
#define IRQF_TRIGGER_RISING 0x1
#define enable_irq_wake(i) 0
#define disable_irq_wake(i) 0
#define devm_ioremap(d, pa, n) lx_ioremap(pa, n)
enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii);

/* ---- the hybrid bus (ahb.c for WCN6750), provided by src/nw_ahb.c over the board ---- */
/* devicetree properties: the only ones the WCN6750 path reads are the remote processor's phandle and the first interrupt's number */
int nw_of_read_u32(const char *name, u32 *out);
int nw_of_read_u32_index(const char *name, u32 index, u32 *out);
#undef of_property_read_u32
#define of_property_read_u32(np, n, out) nw_of_read_u32(n, out)
#define of_property_read_u32_index(np, n, i, out) nw_of_read_u32_index(n, i, out)
#define of_reserved_mem_region_to_resource(np, i, r) (-ENOENT)
#define of_dma_configure(d, np, f) (-ENODEV)
/* the platform device: one, the WCN6750 */
#define IORESOURCE_MEM 0x200
struct resource *nw_platform_resource(unsigned int type, unsigned int index);
#define platform_get_resource(p, t, i) nw_platform_resource(t, i)
int nw_platform_irq(unsigned int index);
#define platform_get_irq(p, i) nw_platform_irq(i)
#define platform_get_irq_byname(p, n) (-ENXIO)
#define platform_get_drvdata(p) dev_get_drvdata(&(p)->dev)
#define platform_set_drvdata(p, d) dev_set_drvdata(&(p)->dev, d)
const void *nw_match_data(void);
#define device_get_match_data(d) nw_match_data()
#define devm_platform_get_and_ioremap_resource(p, i, r) ((void *)ERR_PTR(-ENODEV))
struct platform_device_info { struct device *parent; void *fwnode; const char *name; u64 dma_mask; };
#define platform_device_register_full(i) ((struct platform_device *)ERR_PTR(-ENODEV))
#define platform_device_unregister(p) ((void)0)
#define to_platform_device(d) container_of(d, struct platform_device, dev)
int nw_platform_driver_register(struct platform_driver *drv);
void nw_platform_driver_unregister(struct platform_driver *drv);
#define module_platform_driver(drv) \
  int lx_register_##drv(void) { return nw_platform_driver_register(&drv); } \
  void lx_unregister_##drv(void) { nw_platform_driver_unregister(&drv); }
/* the MSI target is a GIC register: the device reaches it untranslated */
#define dma_map_resource(d, pa, n, dir, attrs) ((dma_addr_t)(pa))
/* the remote processor: the board starts the WPSS before the module; boot waits for its router link */
struct rproc *nw_rproc_get(u32 phandle);
int nw_rproc_boot(struct rproc *r);
void nw_rproc_shutdown(struct rproc *r);
#define rproc_get_by_phandle(p) nw_rproc_get(p)
#define rproc_boot(r) nw_rproc_boot(r)
#define rproc_shutdown(r) nw_rproc_shutdown(r)
#define rproc_put(r) ((void)0)
/* SMP2P out state "wlan" */
struct qcom_smem_state *nw_smem_state_get(const char *name, unsigned int *bit);
int nw_smem_state_update_bits(struct qcom_smem_state *s, u32 mask, u32 value);
#define qcom_smem_state_get(d, n, b) nw_smem_state_get(n, b)
#define qcom_smem_state_update_bits(s, m, v) nw_smem_state_update_bits(s, m, v)
#define qcom_smem_state_put(s) ((void)0)
#endif
