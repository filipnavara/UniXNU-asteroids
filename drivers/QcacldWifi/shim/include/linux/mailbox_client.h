/* QcacldWifi: mailbox client types icnss2's private data carries (the WPSS boot mailbox is not used on this port). */
#ifndef QCACLD_MAILBOX_CLIENT_H
#define QCACLD_MAILBOX_CLIENT_H
struct mbox_chan;
struct mbox_client { struct device *dev; bool tx_block; unsigned long tx_tout; bool knows_txdone;
  void (*rx_callback)(struct mbox_client *cl, void *mssg); void (*tx_prepare)(struct mbox_client *cl, void *mssg);
  void (*tx_done)(struct mbox_client *cl, void *mssg, int r); };
#endif
