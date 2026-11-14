/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The WCN6750's Bluetooth firmware, read where Android keeps it: image/ of the FAT volume on the bluetooth_a (else bluetooth_b) partition
 * (vendor_boot fstab: /dev/block/bootdevice/by-name/bluetooth on /vendor/bt_firmware, vfat, slotselect), read in place through the UFS
 * driver as AdspFirmware.c reads the modem partition; nothing is installed. A file the add-on carries under firmware/qca (VFS, after
 * the add-on mounted) is the fallback. From UniXNU-sheng package/BluetoothFirmware.c (the add-on reader); the names are WCN6750's. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "BluetoothFirmware.h"
#include "FatVolume.h"
extern volatile int native_gpu_addon_stage;
extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) __attribute__((weak_import));
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));
volatile uint32_t native_bluetooth_firmware_source;
char native_bluetooth_files[256];

static bool hex_digit(char c) {return (c>='0' && c<='9') || (c>='a' && c<='f');}
/* qca/msbtfwNN.mbn, qca/msbtfwNN.tlv (btqca.c's WCN6750 rampatch and its fallback), qca/msnvNN.bin (its NVM). */
static bool firmware_name(const char *name) {
    if(!name || strncmp(name,"qca/",4))return false;
    name+=4;
    if(strlen(name)==12 && !strncmp(name,"msbtfw",6) && hex_digit(name[6]) && hex_digit(name[7]) &&
        (!strcmp(name+8,".mbn") || !strcmp(name+8,".tlv")))return true;
    return strlen(name)==10 && !strncmp(name,"msnv",4) && hex_digit(name[4]) && hex_digit(name[5]) && !strcmp(name+6,".bin");
}

/* ---- the bluetooth partition (one reader: the Bluetooth worker thread) ---- */
struct partition {uint32_t lun;uint64_t first;uint64_t cached;uint8_t block[4096];};
static struct partition *gPart;
static FatVolume *gVolume;
static int partitionRead(void *context,uint64_t offset,void *to,size_t length) {
    struct partition *p=context;uint8_t *out=to;
    while(length) {
        const uint64_t lba=offset/4096;const size_t within=(size_t)(offset%4096);
        if(!within && length>=4096) {
            const uint32_t n=length/4096>256?256:(uint32_t)(length/4096);
            if(!native_ufs_part_read(p->lun,p->first+lba,n,out))return EIO;
            out+=(size_t)n*4096;offset+=(uint64_t)n*4096;length-=(size_t)n*4096;
            continue;
        }
        if(p->cached!=lba) {
            p->cached=UINT64_MAX;
            if(!native_ufs_part_read(p->lun,p->first+lba,1,p->block))return EIO;
            p->cached=lba;
        }
        const size_t take=4096-within<length?4096-within:length;
        memcpy(out,p->block+within,take);out+=take;offset+=take;length-=take;
    }
    return 0;
}
static int mountPartition(void) {
    static const char *const names[]={"bluetooth_a","bluetooth_b"};
    if(gVolume)return 0;
    if(!native_ufs_part_find || !native_ufs_part_read)return ENXIO;
    struct partition *p=IOMallocType(struct partition);FatVolume *v=IOMallocType(FatVolume);
    int rc=ENOENT;
    for(unsigned i=0;i<2 && rc;i++) {
        uint64_t blocks=0;
        if(!native_ufs_part_find(names[i],&p->lun,&p->first,&blocks)){rc=ENXIO;continue;}
        p->cached=UINT64_MAX;
        rc=fat_mount(v,blocks*4096,partitionRead,p)?EFTYPE:0;
    }
    if(rc){IOFreeType(p,struct partition);IOFreeType(v,FatVolume);return rc;}
    gPart=p;gVolume=v;
    char *all=IOMallocData(1024);
    if(all) {
        if(!fat_list(gVolume,"image",all,1024))strlcpy(native_bluetooth_files,all,sizeof(native_bluetooth_files));
        IOFreeData(all,1024);
    }
    return 0;
}
static int partitionFile(const char *name,uint8_t **data,size_t *size) {
    int rc=mountPartition();if(rc)return rc;
    char path[48];snprintf(path,sizeof(path),"image/%s",name);
    FatFile file;rc=fat_open(gVolume,path,&file);
    if(rc==FatNotFound)return ENOENT;
    if(rc)return EIO;
    if(file.directory || !file.size || file.size>4u*1024*1024)return EINVAL;
    uint8_t *bytes=IOMallocData(file.size);if(!bytes)return ENOMEM;
    if(fat_read(gVolume,&file,0,bytes,file.size)){IOFreeData(bytes,file.size);return EIO;}
    *data=bytes;*size=file.size;return 0;
}

/* ---- the add-on (sheng's reader) ---- */
static int addonFile(const char *name,uint8_t **data,size_t *size) {
    if(native_gpu_addon_stage<1)return EAGAIN;
    char path[160];snprintf(path,sizeof(path),"%s/%s",SHENG_BT_FIRMWARE_ROOT,name);
    vfs_context_t ctx=vfs_context_kernel();vnode_t vp=NULL;
    int error=vnode_open(path,FREAD,0,0,&vp,ctx);uint8_t *buffer=NULL;size_t length=0;
    if(!error) {
        struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
        error=vnode_getattr(vp,&va,ctx);
        if(!error && (vnode_vtype(vp)!=VREG || !va.va_data_size || va.va_data_size>4u*1024*1024))error=EINVAL;
        if(!error) {
            length=(size_t)va.va_data_size;buffer=IOMallocData(length);
            if(!buffer)error=ENOMEM;
            else {
                int resid=0;
                error=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,(int)length,0,UIO_SYSSPACE,IO_NOAUTH,
                    vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
                if(!error && resid)error=EIO;
            }
        }
    }
    if(vp)vnode_close(vp,FREAD,ctx);
    if(error){if(buffer)IOFreeData(buffer,length);return error;}
    *data=buffer;*size=length;return 0;
}

int sheng_bt_firmware_read(const char *name,uint8_t **data,size_t *size) {
    if(!data || !size)return EINVAL;
    *data=NULL;*size=0;
    if(!firmware_name(name))return ENOENT;
    int error=partitionFile(name+4,data,size);
    if(!error){native_bluetooth_firmware_source=1;return 0;}
    int addon=addonFile(name+4,data,size);
    if(!addon){native_bluetooth_firmware_source=2;return 0;}
    /* The partition's answer, unless it has no such file and the add-on has a reason of its own. */
    return error==ENOENT && addon!=ENOENT?addon:error;
}
void sheng_bt_firmware_release(uint8_t *data,size_t size) {if(data)IOFreeData(data,size);}
