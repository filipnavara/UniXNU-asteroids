/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* asteroids: WiFi firmware files an owner installed on the Data volume, below /private/var/firmware/ath11k/WCN6750/hw1.0 (ath11k's own
 * directory name), read through VFS on the WiFi worker. Built with kern_exec's BSD flags, as AdspFirmware.c. sheng's WifiFirmware.c
 * reader with this board's directory and without its file-name list. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>

#define ASTEROIDS_WIFI_FIRMWARE_ROOT "/private/var/firmware/ath11k/WCN6750/hw1.0"

int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size);

int asteroids_wifi_vfs_read(const char *name, uint8_t **data, size_t *size)
{
    *data = NULL; *size = 0;
    if (!name[0] || strchr(name, '/')) return EINVAL;
    char path[160];
    snprintf(path, sizeof path, "%s/%s", ASTEROIDS_WIFI_FIRMWARE_ROOT, name);
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULL;
    int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    uint8_t *buffer = NULL;
    size_t length = 0;
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        error = vnode_getattr(vp, &va, ctx);
        if (!error && (vnode_vtype(vp) != VREG || !va.va_data_size || va.va_data_size > 16u * 1024 * 1024)) error = EINVAL;
        if (!error) {
            length = (size_t)va.va_data_size;
            buffer = IOMallocData(length);
            if (!buffer) error = ENOMEM;
            else {
                int resid = 0;
                error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                                vfs_context_proc(ctx));
                if (!error && resid) error = EIO;
            }
        }
    }
    if (vp) vnode_close(vp, FREAD, ctx);
    if (error) {if (buffer) IOFreeData(buffer, length); return error;}
    *data = buffer; *size = length;
    return 0;
}
