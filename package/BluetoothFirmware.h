/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The WCN6750's Bluetooth firmware (btqca.c names qca/msbtfwNN.mbn, qca/msbtfwNN.tlv, qca/msnvNN.bin). From UniXNU-sheng
 * package/BluetoothFirmware.h; asteroids reads the files in place from the device's own bluetooth_a/b partition (Android's
 * /vendor/bt_firmware), else from the add-on's firmware/qca. */
#ifndef SHENG_BLUETOOTH_FIRMWARE_H
#define SHENG_BLUETOOTH_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
/* stage-bluetooth-firmware.py consumes this exact addon-relative prefix. */
#define SHENG_BT_FIRMWARE_PREFIX "firmware/qca"
#define SHENG_BT_FIRMWARE_ROOT "/private/var/ngpu/" SHENG_BT_FIRMWARE_PREFIX
#ifdef __cplusplus
extern "C" {
#endif
/* 0 or a positive errno; *data is IOMallocData'd, released with sheng_bt_firmware_release. */
int sheng_bt_firmware_read(const char *name, uint8_t **data, size_t *size);
void sheng_bt_firmware_release(uint8_t *data, size_t size);
/* For the log: where the last file came from (0 none, 1 the bluetooth partition, 2 the add-on) and the partition's image/ names. */
extern volatile uint32_t native_bluetooth_firmware_source;
extern char native_bluetooth_files[256];
#ifdef __cplusplus
}
#endif
#endif
