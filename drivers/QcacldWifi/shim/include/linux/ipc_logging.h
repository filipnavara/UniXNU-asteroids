/* QcacldWifi: no IPC log buffers on this port; icnss2's debug macros also print through the board log. */
#ifndef QCACLD_IPC_LOGGING_H
#define QCACLD_IPC_LOGGING_H
#define ipc_log_context_create(pages, name, id) ((void *)NULL)
#define ipc_log_context_destroy(ctx) ((void)0)
#define ipc_log_string(ctx, fmt, ...) ((void)(ctx))
#endif
