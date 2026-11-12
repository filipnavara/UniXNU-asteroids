/* QcacldWifi: the icnss2 platform API qcacld's pld_ipci uses, on the board's pieces instead of icnss2's main.c/power.c/genl.c.
 * The WLFW QMI client is the vendor's icnss2 qmi.c, unchanged: it works on this file's struct icnss_priv (vendor main.h) and
 * posts its events here. What the vendor gets from the devicetree, regulators, clocks and remoteproc comes from the board:
 * the WPSS is booted by the board (rproc_up), SMP2P goes through nw_board.smp2p_send/soc_wake_in, the MSI layout is icnss2's
 * WCN6750 table on the board's interrupt numbers, the IOVA/MSA ranges are the board's nw_wifi_ddr_range.
 * Sequence (icnss2 main.c for the WCN6750): register driver -> WLFW server arrive -> ind register, host cap, cap, device info
 * (register window, MHI state page), regdb + BDF download -> FW init done -> driver probe -> wlan_enable (WLAN_CFG, WLAN_MODE). */
#include "lx_pci.h"
#include <linux/soc/qcom/qmi.h>
#include <soc/qcom/icnss2.h>
#include "cnss_common.h"
#include "wlan_firmware_service_v01.h"
#include "main.h"
#include "qmi.h"
#include "debug.h"
#include "nw_board.h"

#define ICNSS_MAGIC 0x5abc5abc
#define ICNSS_EVENT_SYNC BIT(0)
#define ICNSS_EVENT_PENDING 2989
#define NW_ICNSS_SOC_WAKE_TIMEOUT_MS 500   /* icnss2's SMP2P_SOC_WAKE_TIMEOUT */
#define NW_ICNSS_MAX_PROBE_CNT 2

void nw_board_qrtr_start(void);
extern u64 nw_wifi_ddr_range[6];
extern u64 nw_wifi_mhi_pa;
void nw_wifi_cpr_report(u32 mv);

struct icnss_priv *penv;
static struct icnss_priv g_priv;
static struct platform_device g_pdev;

/* ---- MSI: icnss2's WCN6750 table ---- */
static struct icnss_msi_user g_msi_users[] = {
  { .name = "CE", .num_vectors = 10, .base_vector = 0 },
  { .name = "DP", .num_vectors = 18, .base_vector = 10 },
};
static struct icnss_msi_config g_msi_config = { .total_vectors = 28, .total_users = 2, .users = g_msi_users };

/* ---- events: icnss2's event work, one queue run on the dispatcher ---- */
static LIST_HEAD(g_events);
static DEFINE_SPINLOCK(g_event_lock);
static struct work_struct g_event_work;

static int icnss_call_driver_probe(struct icnss_priv *priv) {
  int ret = 0;
  if (!priv->ops || !priv->ops->probe) return 0;
  if (test_bit(ICNSS_DRIVER_PROBED, &priv->state)) return -EINVAL;
  set_bit(ICNSS_POWER_ON, &priv->state);
  for (int n = 0; n < NW_ICNSS_MAX_PROBE_CNT; n++) {
    ret = priv->ops->probe(&priv->pdev->dev);
    if (ret != -EPROBE_DEFER) break;
  }
  if (ret < 0) { icnss_pr_err("Driver probe failed: %d, state: 0x%lx\n", ret, priv->state); return ret; }
  set_bit(ICNSS_DRIVER_PROBED, &priv->state);
  return 0;
}

static int event_server_arrive(struct icnss_priv *priv, void *data) {
  int ret;
  set_bit(ICNSS_WLFW_EXISTS, &priv->state);
  clear_bit(ICNSS_FW_DOWN, &priv->state);
  clear_bit(ICNSS_FW_READY, &priv->state);
  if (test_bit(ICNSS_WLFW_CONNECTED, &priv->state)) { icnss_pr_err("QMI Server already in Connected State\n"); return -EALREADY; }
  ret = icnss_connect_to_fw_server(priv, data);
  if (ret) goto fail;
  set_bit(ICNSS_WLFW_CONNECTED, &priv->state);
  ret = wlfw_ind_register_send_sync_msg(priv);
  if (ret < 0 && ret != -EALREADY) goto fail;
  ret = wlfw_host_cap_send_sync(priv);
  if (ret < 0) goto fail;
  ret = wlfw_cap_send_sync_msg(priv);
  if (ret < 0) goto fail;
  if (priv->cpr_info.voltage) nw_wifi_cpr_report(priv->cpr_info.voltage);
  set_bit(ICNSS_POWER_ON, &priv->state);
  ret = wlfw_device_info_send_msg(priv);
  if (ret < 0) goto fail;
  priv->mem_base_va = lx_ioremap(priv->mem_base_pa, priv->mem_base_size);
  if (!priv->mem_base_va) { icnss_pr_err("Ioremap failed for bar address\n"); ret = -ENOMEM; goto fail; }
  icnss_pr_dbg("Non-Secured Bar Address pa: %pa, va: 0x%pK\n", &priv->mem_base_pa, priv->mem_base_va);
  if (priv->mhi_state_info_pa) priv->mhi_state_info_va = lx_ioremap(priv->mhi_state_info_pa, PAGE_SIZE);
  if (!priv->mhi_state_info_va) icnss_pr_err("Ioremap failed for MHI info address\n");
  nw_wifi_mhi_pa = priv->mhi_state_info_pa;   /* the board reads the MHI state words around its own diagnostics */
  if (priv->bdf_download_support) {
    icnss_wlfw_bdf_dnld_send_sync(priv, ICNSS_BDF_REGDB);
    ret = icnss_wlfw_bdf_dnld_send_sync(priv, priv->ctrl_params.bdf_type);
    if (ret < 0) goto fail;
  }
  return 0;
fail:
  icnss_pr_err("WLFW server arrive failed: %d, state: 0x%lx\n", ret, priv->state);
  return ret;
}

static int event_server_exit(struct icnss_priv *priv) {
  icnss_pr_info("WLFW server exit: state 0x%lx\n", priv->state);
  set_bit(ICNSS_FW_DOWN, &priv->state);
  icnss_clear_server(priv);
  return 0;
}

static int event_fw_ready(struct icnss_priv *priv) {
  set_bit(ICNSS_FW_READY, &priv->state);
  clear_bit(ICNSS_MODE_ON, &priv->state);
  atomic_set(&priv->soc_wake_ref_count, 0);
  icnss_pr_info("WLAN FW is ready: 0x%lx\n", priv->state);
  return icnss_call_driver_probe(priv);
}

static int event_fw_init_done(struct icnss_priv *priv) {
  icnss_pr_info("WLAN FW Initialization done: 0x%lx\n", priv->state);
  return event_fw_ready(priv);   /* no QDSS config, no cold boot calibration on this port */
}

static void event_work(struct work_struct *w) {
  (void)w;
  for (;;) {
    unsigned long f;
    spin_lock_irqsave(&g_event_lock, f);
    struct icnss_driver_event *ev = list_first_entry_or_null(&g_events, struct icnss_driver_event, list);
    if (ev) list_del(&ev->list);
    spin_unlock_irqrestore(&g_event_lock, f);
    if (!ev) return;
    struct icnss_priv *priv = &g_priv; int ret = 0;
    switch (ev->type) {
    case ICNSS_DRIVER_EVENT_SERVER_ARRIVE: ret = event_server_arrive(priv, ev->data); break;
    case ICNSS_DRIVER_EVENT_SERVER_EXIT: ret = event_server_exit(priv); break;
    case ICNSS_DRIVER_EVENT_FW_READY_IND: ret = event_fw_ready(priv); break;
    case ICNSS_DRIVER_EVENT_FW_INIT_DONE_IND: ret = event_fw_init_done(priv); break;
    default: icnss_pr_dbg("Event %d ignored on this port\n", ev->type); break;
    }
    priv->stats.events[ev->type].processed++;
    if (ev->sync) { ev->ret = ret; complete(&ev->complete); }
    else kfree(ev);
  }
}

int icnss_driver_event_post(struct icnss_priv *priv, enum icnss_driver_event_type type, u32 flags, void *data) {
  if (!priv) return -ENODEV;
  if (type >= ICNSS_DRIVER_EVENT_MAX) return -EINVAL;
  struct icnss_driver_event *ev = kzalloc(sizeof *ev, GFP_ATOMIC);
  if (!ev) return -ENOMEM;
  ev->type = type; ev->data = data; init_completion(&ev->complete);
  ev->ret = ICNSS_EVENT_PENDING; ev->sync = !!(flags & ICNSS_EVENT_SYNC);
  unsigned long f;
  spin_lock_irqsave(&g_event_lock, f); list_add_tail(&ev->list, &g_events); spin_unlock_irqrestore(&g_event_lock, f);
  priv->stats.events[type].posted++;
  schedule_work(&g_event_work);
  if (!ev->sync) return 0;
  wait_for_completion(&ev->complete);
  int ret = ev->ret; kfree(ev);
  return ret;
}

/* ---- what qmi.c gets from main.c / power.c / genl.c ---- */
int icnss_call_driver_uevent(struct icnss_priv *priv, enum icnss_uevent uevent, void *data) {
  if (!priv->ops || !priv->ops->uevent) return 0;
  struct icnss_uevent_data d = { .uevent = uevent, .data = data };
  return priv->ops->uevent(&priv->pdev->dev, &d);
}
int icnss_get_iova(struct icnss_priv *priv, u64 *addr, u64 *size) {
  if (!priv) return -ENODEV;
  if (!priv->smmu_iova_len) return -EINVAL;
  *addr = priv->smmu_iova_start; *size = priv->smmu_iova_len; return 0;
}
/* The IPA window adds to the first ddr_range (icnss2: iova size + ipa size); a board without one sends size 0. */
int icnss_get_iova_ipa(struct icnss_priv *priv, u64 *addr, u64 *size) {
  if (!priv) return -ENODEV;
  if (!priv->smmu_iova_len) return -EINVAL;
  *addr = priv->smmu_iova_ipa_start; *size = priv->smmu_iova_ipa_len; return 0;
}
int icnss_update_cpr_info(struct icnss_priv *priv) {
  if (priv->cpr_info.voltage) nw_wifi_cpr_report(priv->cpr_info.voltage);
  return 0;
}
void icnss_add_fw_prefix_name(struct icnss_priv *priv, char *prefix_name, char *name) {
  if (!priv) return;
  if (!priv->use_prefix_path) { scnprintf(prefix_name, ICNSS_MAX_FILE_NAME, "%s", name); return; }
  scnprintf(prefix_name, ICNSS_MAX_FILE_NAME, QCA6750_PATH_PREFIX "%s", name);
}
int icnss_genl_send_msg(void *buff, u8 type, char *file_name, u32 total_size) {
  (void)buff; (void)type; (void)file_name; (void)total_size; return -EOPNOTSUPP;   /* no cnss daemon on this port */
}

/* ---- SMP2P (icnss2's icnss_send_smp2p): message (seq << 16 | id) on the board's outbound entry; soc wake waits for the ack ---- */
static int send_smp2p(struct icnss_priv *priv, enum icnss_smp2p_msg_id msg_id, enum smp2p_out_entry entry) {
  if (!nw_board || !nw_board->smp2p_send) return -ENODEV;
  if (msg_id == ICNSS_RESET_MSG) { priv->smp2p_info[entry].seq = 0; return nw_board->smp2p_send(entry, 0) < 0 ? -EIO : 0; }
  if (test_bit(ICNSS_FW_DOWN, &priv->state) || !test_bit(ICNSS_FW_READY, &priv->state)) return -EINVAL;
  u32 value = ((u32)priv->smp2p_info[entry].seq++ << ICNSS_SMEM_SEQ_NO_POS) | msg_id;
  int32_t before = nw_board->soc_wake_in ? nw_board->soc_wake_in() : 0;
  if (nw_board->smp2p_send(entry, value) < 0) { icnss_pr_err("Error in SMP2P send: entry %d value 0x%x\n", entry, value); return -EIO; }
  if ((msg_id == ICNSS_SOC_WAKE_REQ || msg_id == ICNSS_SOC_WAKE_REL) && nw_board->soc_wake_in) {
    u64 start = lx_now_ms();
    while (nw_board->soc_wake_in() == before) {
      if (lx_now_ms() - start >= NW_ICNSS_SOC_WAKE_TIMEOUT_MS) { icnss_pr_err("SMP2P Soc Wake timeout msg %d\n", msg_id); break; }
      if (!lx_pump()) lx_idle(1);
    }
  }
  return 0;
}

/* ---- the platform API (soc/qcom/icnss2.h) ---- */
static struct icnss_priv *dev_priv(struct device *dev) {
  struct icnss_priv *p = dev ? dev_get_drvdata(dev) : NULL;
  return p && p->magic == ICNSS_MAGIC ? p : NULL;
}

static void priv_init(void) {
  struct icnss_priv *priv = &g_priv;
  if (priv->magic == ICNSS_MAGIC) return;
  priv->magic = ICNSS_MAGIC;
  device_initialize(&g_pdev.dev);
  dev_set_name(&g_pdev.dev, "wcn6750");
  g_pdev.dev.dma_mask = 0xffffffffull;
  priv->pdev = &g_pdev;
  dev_set_drvdata(&g_pdev.dev, priv);
  priv->device_id = WCN6750_DEVICE_ID;
  priv->ctrl_params.qmi_timeout = msecs_to_jiffies(3000);
  priv->ctrl_params.quirks = BIT(FW_REJUVENATE_ENABLE);
  priv->ctrl_params.bdf_type = ICNSS_BDF_ELF;
  priv->wpss_supported = true;
  priv->bdf_download_support = true;        /* stock: regdb.bin then bdwlan.elf */
  priv->use_prefix_path = true;             /* stock devicetree: qcom,fw-prefix */
  priv->msi_config = &g_msi_config;
  if (nw_board) {
    priv->msi_addr_pa = nw_board->msi_addr; priv->msi_addr_iova = nw_board->msi_addr;
    priv->msi_base_data = nw_board->msi_spi_first + 32;
    for (int i = 0; i < g_msi_config.total_vectors && i < (int)ARRAY_SIZE(priv->srng_irqs); i++) priv->srng_irqs[i] = nw_board->irq_base + i;
  }
  priv->smmu_iova_start = nw_wifi_ddr_range[0]; priv->smmu_iova_len = nw_wifi_ddr_range[1];
  priv->msa_pa = nw_wifi_ddr_range[2]; priv->msa_mem_size = (u32)nw_wifi_ddr_range[3];
  priv->smmu_iova_ipa_start = nw_wifi_ddr_range[4]; priv->smmu_iova_ipa_len = nw_wifi_ddr_range[5];
  penv = priv;
}

int __icnss_register_driver(struct icnss_driver_ops *ops, struct module *owner, const char *mod_name) {
  (void)owner; (void)mod_name;
  priv_init();
  struct icnss_priv *priv = &g_priv;
  if (priv->ops) return -EEXIST;
  if (!ops || !ops->probe || !ops->remove) return -EINVAL;
  priv->ops = ops;
  INIT_WORK(&g_event_work, event_work);
  if (!nw_board || !nw_board->rproc_up) return -ENODEV;
  if (!nw_board->rproc_up(30000)) { icnss_pr_err("the WPSS is not running\n"); priv->ops = NULL; return -ETIMEDOUT; }
  nw_board_qrtr_start();
  int ret = icnss_register_fw_service(priv);   /* the WLFW lookup: server arrive comes as an event */
  if (ret) { icnss_pr_err("WLFW service registration failed: %d\n", ret); priv->ops = NULL; }
  return ret;
}

int icnss_unregister_driver(struct icnss_driver_ops *ops) {
  struct icnss_priv *priv = &g_priv;
  if (!priv->ops || priv->ops != ops) return -EINVAL;
  if (test_bit(ICNSS_DRIVER_PROBED, &priv->state) && ops->remove) ops->remove(&priv->pdev->dev);
  clear_bit(ICNSS_DRIVER_PROBED, &priv->state);
  icnss_unregister_fw_service(priv);
  priv->ops = NULL;
  return 0;
}

int icnss_wlan_enable(struct device *dev, struct icnss_wlan_enable_cfg *config, enum icnss_driver_mode mode,
                      const char *host_version) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -ENODEV;
  if (test_bit(ICNSS_FW_DOWN, &priv->state) || !test_bit(ICNSS_FW_READY, &priv->state)) return -EINVAL;
  if (test_bit(ICNSS_MODE_ON, &priv->state)) { icnss_pr_err("Already Mode on, ignoring wlan_enable state: 0x%lx\n", priv->state); return -EINVAL; }
  return icnss_send_wlan_enable_to_fw(priv, config, mode, host_version);
}

int icnss_wlan_disable(struct device *dev, enum icnss_driver_mode mode) {
  (void)mode;
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -ENODEV;
  if (test_bit(ICNSS_FW_DOWN, &priv->state)) return 0;
  send_smp2p(priv, ICNSS_RESET_MSG, ICNSS_SMP2P_OUT_SOC_WAKE);
  send_smp2p(priv, ICNSS_RESET_MSG, ICNSS_SMP2P_OUT_EP_POWER_SAVE);
  return icnss_send_wlan_disable_to_fw(priv);
}

int icnss_get_soc_info(struct device *dev, struct icnss_soc_info *info) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  info->v_addr = priv->mem_base_va;
  info->p_addr = priv->mem_base_pa;
  info->chip_id = priv->chip_info.chip_id;
  info->chip_family = priv->chip_info.chip_family;
  info->board_id = priv->board_id;
  info->soc_id = priv->soc_id;
  info->fw_version = priv->fw_version_info.fw_version;
  priv->fw_version_info.fw_build_timestamp[WLFW_MAX_TIMESTAMP_LEN] = '\0';
  strlcpy(info->fw_build_timestamp, priv->fw_version_info.fw_build_timestamp, WLFW_MAX_TIMESTAMP_LEN + 1);
  strlcpy(info->fw_build_id, priv->fw_build_id, ICNSS_WLFW_MAX_BUILD_ID_LEN + 1);
  info->rd_card_chain_cap = priv->rd_card_chain_cap;
  info->phy_he_channel_width_cap = priv->phy_he_channel_width_cap;
  info->phy_qam_cap = priv->phy_qam_cap;
  memcpy(&info->dev_mem_info, &priv->dev_mem_info, sizeof info->dev_mem_info);
  return 0;
}

int icnss_get_user_msi_assignment(struct device *dev, char *user_name, int *num_vectors, u32 *user_base_data, u32 *base_vector) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !priv->msi_config) return -ENODEV;
  for (int i = 0; i < priv->msi_config->total_users; i++)
    if (!strcmp(user_name, priv->msi_config->users[i].name)) {
      *num_vectors = priv->msi_config->users[i].num_vectors;
      *user_base_data = priv->msi_config->users[i].base_vector + priv->msi_base_data;
      *base_vector = priv->msi_config->users[i].base_vector;
      return 0;
    }
  return -ENODEV;
}
int icnss_get_msi_irq(struct device *dev, unsigned int vector) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || vector >= (unsigned)priv->msi_config->total_vectors) return -EINVAL;
  return priv->srng_irqs[vector];
}
void icnss_get_msi_address(struct device *dev, u32 *msi_addr_low, u32 *msi_addr_high) {
  struct icnss_priv *priv = dev_priv(dev);
  *msi_addr_low = priv ? lower_32_bits(priv->msi_addr_iova) : 0;
  *msi_addr_high = priv ? upper_32_bits(priv->msi_addr_iova) : 0;
}

int icnss_force_wake_request(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  if (test_bit(ICNSS_FW_DOWN, &priv->state) || !test_bit(ICNSS_FW_READY, &priv->state)) return -EINVAL;
  if (atomic_inc_not_zero(&priv->soc_wake_ref_count)) return 0;
  int ret = send_smp2p(priv, ICNSS_SOC_WAKE_REQ, ICNSS_SMP2P_OUT_SOC_WAKE);
  if (!ret) atomic_inc(&priv->soc_wake_ref_count);
  return ret;
}
int icnss_force_wake_release(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  if (test_bit(ICNSS_FW_DOWN, &priv->state) || !test_bit(ICNSS_FW_READY, &priv->state)) return -EINVAL;
  if (atomic_dec_if_positive(&priv->soc_wake_ref_count)) return 0;   /* still held by another user (icnss2's order) */
  return send_smp2p(priv, ICNSS_SOC_WAKE_REL, ICNSS_SMP2P_OUT_SOC_WAKE);
}
int icnss_is_device_awake(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  return priv ? atomic_read(&priv->soc_wake_ref_count) : -EINVAL;
}
int icnss_get_mhi_state(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  if (!priv->mhi_state_info_va) return -ENOMEM;
  return ioread32(priv->mhi_state_info_va);
}
int icnss_is_pci_ep_awake(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  if (!priv->mhi_state_info_va) return -ENOMEM;
  return ioread32((u8 *)priv->mhi_state_info_va + ICNSS_PCI_EP_WAKE_OFFSET);
}
int icnss_exit_power_save(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !test_bit(ICNSS_MODE_ON, &priv->state)) return 0;
  return send_smp2p(priv, ICNSS_POWER_SAVE_EXIT, ICNSS_SMP2P_OUT_POWER_SAVE);
}
int icnss_prevent_l1(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !test_bit(ICNSS_MODE_ON, &priv->state)) return 0;
  return send_smp2p(priv, ICNSS_PCI_EP_POWER_SAVE_EXIT, ICNSS_SMP2P_OUT_EP_POWER_SAVE);
}
void icnss_allow_l1(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !test_bit(ICNSS_MODE_ON, &priv->state)) return;
  send_smp2p(priv, ICNSS_PCI_EP_POWER_SAVE_ENTER, ICNSS_SMP2P_OUT_EP_POWER_SAVE);
}

int icnss_set_fw_log_mode(struct device *dev, uint8_t fw_log_mode) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv) return -EINVAL;
  if (test_bit(ICNSS_FW_DOWN, &priv->state) || !test_bit(ICNSS_FW_READY, &priv->state)) return -EINVAL;
  return wlfw_ini_send_sync_msg(priv, fw_log_mode);
}
int icnss_athdiag_read(struct device *dev, uint32_t offset, uint32_t mem_type, uint32_t data_len, uint8_t *output) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !output || !data_len || data_len > WLFW_MAX_DATA_SIZE) return -EINVAL;
  if (!test_bit(ICNSS_FW_READY, &priv->state) || !test_bit(ICNSS_POWER_ON, &priv->state)) return -EINVAL;
  return wlfw_athdiag_read_send_sync_msg(priv, offset, mem_type, data_len, output);
}
int icnss_athdiag_write(struct device *dev, uint32_t offset, uint32_t mem_type, uint32_t data_len, uint8_t *input) {
  struct icnss_priv *priv = dev_priv(dev);
  if (!priv || !input || !data_len || data_len > WLFW_MAX_DATA_SIZE) return -EINVAL;
  if (!test_bit(ICNSS_FW_READY, &priv->state) || !test_bit(ICNSS_POWER_ON, &priv->state)) return -EINVAL;
  return wlfw_athdiag_write_send_sync_msg(priv, offset, mem_type, data_len, input);
}

/* ---- not on this port ---- */
int icnss_qmi_send(struct device *dev, int type, void *cmd, int cmd_len, void *cb_ctx, int (*cb)(void *, void *, int)) {
  (void)dev; (void)type; (void)cmd; (void)cmd_len; (void)cb_ctx; (void)cb; return -EOPNOTSUPP;
}
int icnss_power_on(struct device *dev) { (void)dev; return 0; }    /* the board owns the rails */
int icnss_power_off(struct device *dev) { (void)dev; return 0; }
struct iommu_domain *icnss_smmu_get_domain(struct device *dev) { (void)dev; return NULL; }   /* no S1 translation: bus = DMA window */
int icnss_smmu_map(struct device *dev, phys_addr_t paddr, uint32_t *iova_addr, size_t size) {
  (void)dev; (void)paddr; (void)iova_addr; (void)size; return -EOPNOTSUPP;
}
int icnss_thermal_cdev_register(struct device *dev, unsigned long max_state, int tcdev_id) {
  (void)dev; (void)max_state; (void)tcdev_id; return 0;
}
void icnss_thermal_cdev_unregister(struct device *dev, int tcdev_id) { (void)dev; (void)tcdev_id; }
int icnss_get_curr_therm_cdev_state(struct device *dev, unsigned long *thermal_state, int tcdev_id) {
  (void)dev; (void)tcdev_id; *thermal_state = 0; return 0;
}
int icnss_idle_shutdown(struct device *dev) { (void)dev; return -EOPNOTSUPP; }
int icnss_idle_restart(struct device *dev) { (void)dev; return -EOPNOTSUPP; }
int icnss_trigger_recovery(struct device *dev) {
  struct icnss_priv *priv = dev_priv(dev);
  icnss_pr_err("Recovery requested (not on this port): state 0x%lx\n", priv ? priv->state : 0);
  return -EOPNOTSUPP;
}
bool icnss_is_fw_down(void) { return penv && test_bit(ICNSS_FW_DOWN, &penv->state); }
bool icnss_is_fw_ready(void) { return penv && test_bit(ICNSS_FW_READY, &penv->state); }
