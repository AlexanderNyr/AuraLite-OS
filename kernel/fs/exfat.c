#include "kernel/fs/exfat.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"

static int exfat_dev_id = -1;
static struct exfat_boot_region boot_region;

/* Helper to read a cluster into a buffer */
static struct buffer* exfat_get_cluster_buf(uint64_t cluster) {
    uint64_t sector = (cluster * boot_region.sectors_per_cluster);
    return bc_get(exfat_dev_id, sector);
}

struct exfat_vnode {
    uint64_t inode_id; /* Cluster number */
    uint64_t size;
    uint32_t type;
};

static struct vnode* exfat_lookup(void *fs_data, const char *path) {
    /* Simplified lookup: starts from root_dir_cluster and scans entries */
    struct buffer *dir_buf = exfat_get_cluster_buf(boot_region.root_dir_cluster);
    if (!dir_buf) return NULL;

    struct exfat_dir_entry *entries = (struct exfat_dir_entry *)dir_buf->data;
    for (int i = 0; i < BC_BLOCK_SIZE / sizeof(struct exfat_dir_entry); i++) {
        if (entries[i].type == 0x85 && strcmp((char*)entries[i].name, path) == 0) {
            struct vnode *vn = kmalloc(sizeof(struct vnode));
            vn->type = (entries[i].file_attrs & 0x10) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            vn->size = entries[i].size;
            vn->inode_id = entries[i].first_cluster;
            vn->ops = &exfat_ops;
            vn->fs_data = kmalloc(sizeof(struct exfat_vnode));
            ((struct exfat_vnode*)vn->fs_data)->inode_id = vn->inode_id;
            ((struct exfat_vnode*)vn->fs_data)->size = vn->size;
            
            bc_release(dir_buf);
            return vn;
        }
    }
    bc_release(dir_buf);
    return NULL;
}

static int64_t exfat_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct exfat_vnode *evn = vn->fs_data;
    uint64_t cluster = evn->inode_id;
    uint64_t offset_in_cluster = pos % (boot_region.sectors_per_cluster * BC_BLOCK_SIZE);
    
    struct buffer *b = exfat_get_cluster_buf(cluster);
    if (!b) return -1;

    uint64_t available = BC_BLOCK_SIZE - offset_in_cluster;
    uint64_t to_copy = (count < available) ? count : available;
    
    memcpy(buf, b->data + offset_in_cluster, to_copy);
    bc_release(b);
    return to_copy;
}

static int64_t exfat_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct exfat_vnode *evn = vn->fs_data;
    uint64_t cluster = evn->inode_id;
    uint64_t offset_in_cluster = pos % (boot_region.sectors_per_cluster * BC_BLOCK_SIZE);
    
    struct buffer *b = exfat_get_cluster_buf(cluster);
    if (!b) return -1;

    uint64_t available = BC_BLOCK_SIZE - offset_in_cluster;
    uint64_t to_copy = (count < available) ? count : available;
    
    memcpy(b->data + offset_in_cluster, buf, to_copy);
    b->dirty = true;
    bc_release(b);
    return to_copy;
}

const struct vfs_ops exfat_ops = {
    .lookup = exfat_lookup,
    .read = exfat_read,
    .write = exfat_write,
};

void exfat_init(int device_id) {
    exfat_dev_id = device_id;
    struct buffer *b = bc_get(device_id, 0);
    if (b) {
        memcpy(&boot_region, b->data, sizeof(struct exfat_boot_region));
        bc_release(b);
        kprintf("[fs] exFAT initialised on device %d\n", device_id);
    }
}
