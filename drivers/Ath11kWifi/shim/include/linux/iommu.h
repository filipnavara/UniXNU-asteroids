/* ath11k_ahb maps the firmware's MSA through an IOMMU domain only with a "wifi-firmware" devicetree child; this board has none (the
 * secure world owns the WPSS's memory), so these are never reached. */
#ifndef LX_IOMMU_H
#define LX_IOMMU_H
#include "lx_os.h"
struct iommu_domain { int unused; };
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define iommu_paging_domain_alloc(d) ((struct iommu_domain *)ERR_PTR(-ENODEV))
#define iommu_attach_device(dom, d) (-ENODEV)
#define iommu_detach_device(dom, d) ((void)0)
#define iommu_domain_free(dom) ((void)0)
#define iommu_map(dom, iova, pa, n, prot, gfp) (-ENODEV)
#define iommu_unmap(dom, iova, n) ((size_t)0)
#endif
