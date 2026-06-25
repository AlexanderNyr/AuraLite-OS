#include "kernel/fs/ntfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include <string.h>

static int ntfs_dev_id = -1;
static struct ntfs_boot_sector boot;

static struct buffer* ntfs_get_cluster_buf(uint64_t cluster) {
    uint64_t sector = cluster * boot.sectors_per_cluster;
    return bc_get(ntfs_dev_id, sector);
}

struct ntfs_vnode {
    uint64_t mft_index;
};

static struct vnode* ntfs_lookup(void *fs_data, const char *path) {
    /* Simplified: Look up in MFT starting from root (MFT index 5) */
    struct buffer *b = ntfs_get_cluster_buf(boot.mft_start_cluster);
    if (!b) return NULL;

    struct ntfs_mft_record *rec = (struct ntfs_mft_record *)b->data;
    if (strncmp(rec->magic, "FILE", 4) != 0) {
        bc_release(b);
        return NULL;
    }

    /* In a real NTFS driver, we would iterate attributes to find $FILE_NAME */
    struct vnode *vn = kmalloc(sizeof(struct vnode));
    vn->type = VFS_TYPE_FILE;
    vn->inode_id = 0;
    vn->ops = &ntfs_ops;
    vn->fs_data = kmalloc(sizeof(struct ntfs_vnode));
    ((struct ntfs_vnode*)vn->fs_data)->mft_index = 0;

    bc_release(b);
    return vn;
}

static int64_t ntfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    /* NTFS data is in $DATA attribute runs. Simplified: read from MFT base cluster. */
    struct ntfs_vnode *nvn = vn->fs_data;
    struct buffer *b = ntfs_get_cluster_buf(boot.mft_start_cluster);
    if (!b) return -1;
    
    uint64_t available = BC_BLOCK_SIZE - (pos % BC_BLOCK_SIZE);
    uint64_t to_copy = (count < available) ? count : available;
    memcpy(buf, b->data + (pos % BC_BLOCK_SIZE), to_copy);
    
    bc_release(b);
    return to_copy;
}

const struct vfs_ops ntfs_ops = {
    .lookup = ntfs_lookup,
    .read = ntfs_read,
};

void ntfs_init(int device_id) {
    ntfs_dev_id = device_id;
    struct buffer *b = bc_get(device_id, 0);
    if (b) {
        memcpy(&boot, b->data, sizeof(struct ntfs_boot_sector));
        bc_release(b);
        kprintf("[fs] NTFS initialised on device %d\n", device_id);
    }
}
