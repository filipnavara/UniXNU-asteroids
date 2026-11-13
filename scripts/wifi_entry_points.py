"""The WiFi modules' entry points the board (AsteroidsWifi.cpp) uses: every other global name of a module gets the prefix nwk_.
Used by build-ath11k-wifi.py."""
KEEP = {'_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events', '_nw_wifi_scan', '_nw_wifi_bss_count',
        '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where',
        '_nw_wifi_power',
        '_nw_wifi_tx_packet', '_nw_wifi_link_state',
        '_nw_wifi_tx_reasons', '_nw_wifi_buffer_stats_get', '_nw_wifi_buffer_pool_configure', '_nw_wifi_buffer_pool_trim',
        '_nw_wifi_irq_mode', '_nw_wifi_irq_pending', '_nw_wifi_wait_ms', '_nw_wifi_poll'}
KEEP.update({'_nw_wifi_control_transport_set', '_nw_wifi_control_attach', '_nw_wifi_control_detach',
             '_nw_wifi_control_receive', '_nw_wifi_control_writable', '_nw_wifi_control_stats_get'})
KEEP.update({'_nw_wifi_set_platform', '_nw_wifi_qrtr_rx', '_nw_wifi_qrtr_down', '_nw_wifi_cpr_mv', '_nw_wifi_wmi_mem', '_nw_wifi_idle_ps', '_nw_wifi_self_scan', '_nw_fw_diag_dump', '_nw_wifi_irq_vectors', '_nw_wifi_irq_backstop'})
