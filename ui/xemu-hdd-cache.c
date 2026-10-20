/*
 * xemu-hdd-cache.c -- clear the Xbox HDD's game cache partitions (X, Y, Z)
 * whenever the machine powers on or resets (Settings -> System -> "Clear game
 * cache at startup", sys.clear_cache_on_boot).
 *
 * Why: games copy disc data into the cache partitions while they run, and the
 * Xbox kernel keeps part of the filesystem's bookkeeping in RAM. When xemu is
 * ended abruptly -- on the Xbox (UWP) build the console kills the app; on any
 * build, closing xemu or resetting is a power cut to the guest -- the cache is
 * left half-written. On the next boot the game reads it back as garbage and
 * reports a dirty or damaged disc (seen with KOTOR: boot, kill, boot; clearing
 * the cache from the dashboard fixed it). The cache partitions are scratch
 * space by design, so formatting them at each start is always safe: games
 * rebuild what they need. Saves (partition E) and the system partition (C)
 * are never touched.
 *
 * The format is nxdk's nxFormatVolume (lib/nxdk/format.c, MIT, Stefan
 * Schmidt and GXTX), the routine the xemu dashboard's "Clear Cache" uses:
 * superblock (signature, a new volume id, the same cluster size, root at
 * cluster 1, 0xFF padding), an empty cluster table (entries 0 and 1 reserved),
 * and an empty root directory cluster (0xFF).
 *
 * A partition is only formatted if a FATX superblock already sits at its
 * standard offset; anything else (no signature, odd cluster size) is skipped
 * and logged, so a non-standard disk layout is left alone.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "xemu-settings.h"
#include "xemu-hdd-cache.h"

#define FATX_SIGNATURE 0x58544146 /* "FATX" */
#define SECTOR_SIZE 512
#define PAGE_BYTES 4096

/* The standard Xbox HDD layout: the three cache partitions */
static const struct {
    const char *name;
    int64_t offset, length;
} cache_partitions[] = {
    { "X (Partition3)", 0x00080000LL, 0x2EE00000LL },
    { "Y (Partition4)", 0x2EE80000LL, 0x2EE00000LL },
    { "Z (Partition5)", 0x5DC80000LL, 0x2EE00000LL },
};

static bool format_cache_partition(BlockBackend *blk, int i)
{
    int64_t base = cache_partitions[i].offset;
    int64_t length = cache_partitions[i].length;
    const char *name = cache_partitions[i].name;
    uint8_t page[PAGE_BYTES];
    g_autofree uint8_t *table = NULL;   /* before any goto: cleanup vars */
    g_autofree uint8_t *root = NULL;

    if (blk_pread(blk, base, PAGE_BYTES, page, 0) < 0) {
        fprintf(stderr, "hdd cache: %s: cannot read its superblock\n", name);
        return false;
    }
    uint32_t sig = ldl_le_p(page);
    uint32_t spc = ldl_le_p(page + 8);
    if (sig != FATX_SIGNATURE) {
        fprintf(stderr, "hdd cache: %s: no FATX superblock at 0x%" PRIx64
                " -- left alone\n", name, base);
        return false;
    }
    uint32_t bytes_per_cluster = spc * SECTOR_SIZE;
    if (spc == 0 || (bytes_per_cluster & (bytes_per_cluster - 1)) ||
        bytes_per_cluster < PAGE_BYTES || bytes_per_cluster > 0x40000) {
        fprintf(stderr, "hdd cache: %s: odd cluster size %u -- left alone\n",
                name, bytes_per_cluster);
        return false;
    }

    /* the layout nxFormatVolume writes */
    uint32_t num_clusters = length / bytes_per_cluster + 1;
    bool fatx16 = num_clusters < 65520;
    uint32_t table_bytes = num_clusters * (fatx16 ? 2 : 4);
    table_bytes = (table_bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);

    /* superblock */
    memset(page, 0xff, sizeof(page));
    stl_le_p(page + 0, FATX_SIGNATURE);
    stl_le_p(page + 4, (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    stl_le_p(page + 8, spc);
    stl_le_p(page + 12, 1);      /* root directory: cluster 1 */
    stw_le_p(page + 16, 0);
    if (blk_pwrite(blk, base, PAGE_BYTES, page, 0) < 0) {
        goto write_failed;
    }

    /* cluster table: all free but the two reserved entries */
    table = g_malloc0(table_bytes);
    if (fatx16) {
        stw_le_p(table + 0, 0xfff8);
        stw_le_p(table + 2, 0xffff);
    } else {
        stl_le_p(table + 0, 0xfffffff8);
        stl_le_p(table + 4, 0xffffffff);
    }
    if (blk_pwrite(blk, base + PAGE_BYTES, table_bytes, table, 0) < 0) {
        goto write_failed;
    }

    /* an empty root directory */
    root = g_malloc(bytes_per_cluster);
    memset(root, 0xff, bytes_per_cluster);
    if (blk_pwrite(blk, base + PAGE_BYTES + table_bytes, bytes_per_cluster,
                   root, 0) < 0) {
        goto write_failed;
    }
    return true;

write_failed:
    fprintf(stderr, "hdd cache: %s: write failed\n", name);
    return false;
}

static void clear_cache_on_reset(void *opaque)
{
    (void)opaque;
    if (!g_config.sys.clear_cache_on_boot) {
        return;
    }
    DriveInfo *dinfo = drive_get(IF_IDE, 0, 0);    /* -drive index=0: the HDD */
    BlockBackend *blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    if (!blk || !blk_is_inserted(blk) || !blk_is_writable(blk)) {
        return;
    }
    blk_drain(blk);
    int done = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(cache_partitions); i++) {
        done += format_cache_partition(blk, i);
    }
    blk_flush(blk);
    fprintf(stderr, "hdd cache: cleared %d of 3 cache partitions\n", done);
}

void xemu_hdd_cache_init(void)
{
    qemu_register_reset(clear_cache_on_reset, NULL);
}
