/*
 * Block replication tests
 *
 * Copyright (c) 2016 FUJITSU LIMITED
 * Author: Changlong Xie <xiecl.fnst@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "replication.h"
#include "block/block_int.h"
#include "sysemu/block-backend.h"

#define IMG_SIZE (64 * 1024 * 1024)

/* primary */
#define P_LOCAL_DISK "/tmp/p_local_disk.XXXXXX"
#define P_COMMAND "driver=replication,mode=primary,node-name=xxx,"\
                  "file.driver=qcow2,file.file.filename="P_LOCAL_DISK

/* secondary */
#define S_LOCAL_DISK "/tmp/s_local_disk.XXXXXX"
#define S_ACTIVE_DISK "/tmp/s_active_disk.XXXXXX"
#define S_HIDDEN_DISK "/tmp/s_hidden_disk.XXXXXX"
#define S_ID "secondary-id"
#define S_LOCAL_DISK_ID "secondary-local-disk-id"
#define S_COMMAND1 "file.filename="S_LOCAL_DISK",driver=qcow2"
#define S_COMMAND2 "driver=replication,mode=secondary,top-id="S_ID","\
                   "file.driver=qcow2,file.file.filename="S_ACTIVE_DISK","\
                   "file.backing.driver=qcow2,file.backing.file.filename="\
                   ""S_HIDDEN_DISK",file.backing.backing="S_LOCAL_DISK_ID

/* FIXME: steal from blockdev.c, any better way? */
QemuOptsList qemu_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_drive_opts.head),
    .desc = {
        { /* end of list */ }
    },
};

static void io_read(BlockDriverState *bs, long pattern, int64_t pattern_offset,
                    int64_t pattern_count, int64_t offset, int64_t count,
                    bool expect_failed)
{
    char *buf;
    void *cmp_buf;
    int ret;

    /* 1. alloc pattern buffer */
    if (pattern) {
        cmp_buf = g_malloc(pattern_count);
        memset(cmp_buf, pattern, pattern_count);
    }

    /* 2. alloc read buffer */
    buf = qemu_blockalign(bs, count);
    memset(buf, 0xab, count);

    /* 3. do read */
    ret = bdrv_read(bs, offset >> 9, (uint8_t *)buf, count >> 9);

    /* 4. assert and compare buf */
    if (expect_failed) {
        g_assert(ret < 0);
    } else {
        g_assert(ret >= 0);
        if (pattern) {
            g_assert(memcmp(buf + pattern_offset, cmp_buf, pattern_count) <= 0);
            g_free(cmp_buf);
        }
    }
    g_free(buf);
}

static void io_write(BlockDriverState *bs, long pattern, int64_t pattern_count,
                     int64_t offset, int64_t count, bool expect_failed)
{
    void *pattern_buf;
    int ret;

    /* 1. alloc pattern buffer */
    if (pattern) {
        pattern_buf = g_malloc(pattern_count);
        memset(pattern_buf, pattern, pattern_count);
    }

    /* 2. do write */
    if (pattern) {
        ret = bdrv_write(bs, offset >> 9, (uint8_t *)pattern_buf, count >> 9);
    } else {
        ret = bdrv_write_zeroes(bs, offset >> 9, count >> 9, 0);
    }

    /* 3. assert */
    if (expect_failed) {
        g_assert(ret < 0);
    } else {
        g_assert(ret >= 0);
        g_free(pattern_buf);
    }
}

static void prepare_imgs(void)
{
    Error *local_err = NULL;

    /* Primary */
    bdrv_img_create(P_LOCAL_DISK, "qcow2", NULL, NULL, NULL, IMG_SIZE,
                    BDRV_O_RDWR, &local_err, true);
    g_assert(!local_err);

    /* Secondary */
    bdrv_img_create(S_LOCAL_DISK, "qcow2", NULL, NULL, NULL, IMG_SIZE,
                    BDRV_O_RDWR, &local_err, true);
    g_assert(!local_err);
    bdrv_img_create(S_ACTIVE_DISK, "qcow2", NULL, NULL, NULL, IMG_SIZE,
                    BDRV_O_RDWR, &local_err, true);
    g_assert(!local_err);
    bdrv_img_create(S_HIDDEN_DISK, "qcow2", NULL, NULL, NULL, IMG_SIZE,
                    BDRV_O_RDWR, &local_err, true);
    g_assert(!local_err);
}

static void cleanup_imgs(void)
{
    /* Primary */
    unlink(P_LOCAL_DISK);

    /* Secondary */
    unlink(S_LOCAL_DISK);
    unlink(S_ACTIVE_DISK);
    unlink(S_HIDDEN_DISK);
}

static BlockDriverState *start_primary(void)
{
    BlockDriverState *bs;
    QemuOpts *opts;
    QDict *qdict;
    Error *local_err = NULL;
    int ret;

    /* init Primary BS without BB */
    bs = bdrv_new();
    g_assert(bs);

    opts = qemu_opts_parse_noisily(&qemu_drive_opts, P_COMMAND, false);
    qdict = qemu_opts_to_qdict(opts, NULL);

    qdict_set_default_str(qdict, BDRV_OPT_CACHE_DIRECT, "off");
    qdict_set_default_str(qdict, BDRV_OPT_CACHE_NO_FLUSH, "off");

    ret = bdrv_open(&bs, NULL, NULL, qdict, BDRV_O_RDWR, &local_err);
    g_assert(ret >= 0);
    g_assert(!local_err);

    qemu_opts_del(opts);

    return bs;
}

static void teardown_primary(BlockDriverState *bs)
{
    /* only destroy BS, since we didn't initialize BB in Primary */
    bdrv_unref(bs);
}

static void test_primary_read(void)
{
    BlockDriverState *bs;

    bs = start_primary();
    /* read from 0 to IMG_SIZE */
    io_read(bs, 0, 0, IMG_SIZE, 0, IMG_SIZE, true);

    teardown_primary(bs);
}

static void test_primary_write(void)
{
    BlockDriverState *bs;

    bs = start_primary();
    /* write from 0 to IMG_SIZE */
    io_write(bs, 0, IMG_SIZE, 0, IMG_SIZE, true);

    teardown_primary(bs);
}

static void test_primary_start(void)
{
    BlockDriverState *bs;
    Error *local_err = NULL;

    bs = start_primary();

    replication_start_all(REPLICATION_MODE_PRIMARY, &local_err);
    g_assert(!local_err);
    /* read from 0 to IMG_SIZE */
    io_read(bs, 0, 0, IMG_SIZE, 0, IMG_SIZE, true);

    /* write 0x22 from 0 to IMG_SIZE */
    io_write(bs, 0x22, IMG_SIZE, 0, IMG_SIZE, false);

    teardown_primary(bs);
}

static void test_primary_stop(void)
{
    BlockDriverState *bs;
    Error *local_err = NULL;
    bool failover = true;

    bs = start_primary();

    replication_start_all(REPLICATION_MODE_PRIMARY, &local_err);
    g_assert(!local_err);

    replication_stop_all(failover, &local_err);
    g_assert(!local_err);

    teardown_primary(bs);
}

static void test_primary_do_checkpoint(void)
{
    BlockDriverState *bs;
    Error *local_err = NULL;

    bs = start_primary();

    replication_do_checkpoint_all(&local_err);
    g_assert(!local_err);

    teardown_primary(bs);
}

static void test_primary_get_error(void)
{
    BlockDriverState *bs;
    Error *local_err = NULL;

    bs = start_primary();

    replication_start_all(REPLICATION_MODE_PRIMARY, &local_err);
    g_assert(!local_err);

    replication_get_error_all(&local_err);
    g_assert(!local_err);

    teardown_primary(bs);
}

static BlockDriverState *start_secondary(void)
{
    QemuOpts *opts;
    QDict *qdict;
    BlockBackend *blk;
    BlockDriverState *bs;
    Error *local_err = NULL;

    /* 1. add S_LOCAL_DISK and forge S_LOCAL_DISK_ID */
    opts = qemu_opts_parse_noisily(&qemu_drive_opts, S_COMMAND1, false);
    qdict = qemu_opts_to_qdict(opts, NULL);

    qdict_set_default_str(qdict, BDRV_OPT_CACHE_DIRECT, "off");
    qdict_set_default_str(qdict, BDRV_OPT_CACHE_NO_FLUSH, "off");

    blk = blk_new_open(NULL, NULL, qdict, BDRV_O_RDWR, &local_err);
    assert(blk);
    monitor_add_blk(blk, S_LOCAL_DISK_ID, &local_err);
    g_assert(!local_err);

    /* 2. format S_LOCAL_DISK with pattern "0x11" */
    bs = blk_bs(blk);
    io_write(bs, 0x11, IMG_SIZE, 0, IMG_SIZE, false);

    qemu_opts_del(opts);

    /* 3. add S_(ACTIVE/HIDDEN)_DISK and forge S_ID */
    opts = qemu_opts_parse_noisily(&qemu_drive_opts, S_COMMAND2, false);
    qdict = qemu_opts_to_qdict(opts, NULL);

    qdict_set_default_str(qdict, BDRV_OPT_CACHE_DIRECT, "off");
    qdict_set_default_str(qdict, BDRV_OPT_CACHE_NO_FLUSH, "off");

    blk = blk_new_open(NULL, NULL, qdict, BDRV_O_RDWR, &local_err);
    assert(blk);
    monitor_add_blk(blk, S_ID, &local_err);
    g_assert(!local_err);

    qemu_opts_del(opts);

    /* return top bs */
    return blk_bs(blk);
}

static void teardown_secondary(void)
{
    /* only need to destroy two BBs */
    BlockBackend *blk;

    /* 1. remove S_LOCAL_DISK_ID */
    blk = blk_by_name(S_LOCAL_DISK_ID);
    assert(blk);

    monitor_remove_blk(blk);
    blk_unref(blk);

    /* 2. remove S_ID */
    blk = blk_by_name(S_ID);
    assert(blk);

    monitor_remove_blk(blk);
    blk_unref(blk);
}

static void test_secondary_read(void)
{
    BlockDriverState *top_bs;

    top_bs = start_secondary();
    /* read from 0 to IMG_SIZE */
    io_read(top_bs, 0, 0, IMG_SIZE, 0, IMG_SIZE, true);

    teardown_secondary();
}

static void test_secondary_write(void)
{
    BlockDriverState *bs;

    bs = start_secondary();
    /* write from 0 to IMG_SIZE */
    io_write(bs, 0, IMG_SIZE, 0, IMG_SIZE, true);

    teardown_secondary();
}

static void test_secondary_start(void)
{
    BlockBackend *blk;
    BlockDriverState *top_bs, *local_bs;
    Error *local_err = NULL;
    bool failover = true;

    top_bs = start_secondary();
    replication_start_all(REPLICATION_MODE_SECONDARY, &local_err);
    g_assert(!local_err);

    /* 1. read from S_LOCAL_DISK (0, IMG_SIZE) */
    io_read(top_bs, 0x11, 0, IMG_SIZE, 0, IMG_SIZE, false);

    /* 2. write 0x22 to S_LOCAL_DISK (IMG_SIZE / 2, IMG_SIZE) */
    blk = blk_by_name(S_LOCAL_DISK_ID);
    local_bs = blk_bs(blk);

    io_write(local_bs, 0x22, IMG_SIZE / 2, IMG_SIZE / 2, IMG_SIZE / 2, false);

    /* 2.1 replication will backup S_LOCAL_DISK to S_HIDDEN_DISK */
    io_read(top_bs, 0x11, IMG_SIZE / 2, IMG_SIZE / 2, 0, IMG_SIZE, false);

    /* 3. write 0x33 to S_ACTIVE_DISK (0, IMG_SIZE / 2) */
    io_write(top_bs, 0x33, IMG_SIZE / 2, 0, IMG_SIZE / 2, false);

    /* 3.1 read from S_ACTIVE_DISK (0, IMG_SIZE/2) */
    io_read(top_bs, 0x33, 0, IMG_SIZE / 2, 0, IMG_SIZE / 2, false);

    /* unblock top_bs */
    replication_stop_all(failover, &local_err);
    g_assert(!local_err);

    teardown_secondary();
}

static void test_secondary_stop(void)
{
    BlockBackend *blk;
    BlockDriverState *top_bs, *local_bs;
    Error *local_err = NULL;
    bool failover = true;

    top_bs = start_secondary();
    replication_start_all(REPLICATION_MODE_SECONDARY, &local_err);
    g_assert(!local_err);

    /* 1. write 0x22 to S_LOCAL_DISK (IMG_SIZE / 2, IMG_SIZE) */
    blk = blk_by_name(S_LOCAL_DISK_ID);
    local_bs = blk_bs(blk);

    io_write(local_bs, 0x22, IMG_SIZE / 2, IMG_SIZE / 2, IMG_SIZE / 2, false);

    /* 2. replication will backup S_LOCAL_DISK to S_HIDDEN_DISK */
    io_read(top_bs, 0x11, IMG_SIZE / 2, IMG_SIZE / 2, 0, IMG_SIZE, false);

    /* 3. write 0x33 to S_ACTIVE_DISK (0, IMG_SIZE / 2) */
    io_write(top_bs, 0x33, IMG_SIZE / 2, 0, IMG_SIZE / 2, false);

    /* 4. do active commit */
    replication_stop_all(failover, &local_err);
    g_assert(!local_err);

    /* 5. read from S_LOCAL_DISK (0, IMG_SIZE / 2) */
    io_read(top_bs, 0x33, 0, IMG_SIZE / 2, 0, IMG_SIZE / 2, false);

    /* 6. read from S_LOCAL_DISK (IMG_SIZE / 2, IMG_SIZE) */
    io_read(top_bs, 0x22, IMG_SIZE / 2, IMG_SIZE / 2, 0, IMG_SIZE, false);

    teardown_secondary();
}

static void test_secondary_do_checkpoint(void)
{
    BlockBackend *blk;
    BlockDriverState *top_bs, *local_bs;
    Error *local_err = NULL;
    bool failover = true;

    top_bs = start_secondary();
    replication_start_all(REPLICATION_MODE_SECONDARY, &local_err);
    g_assert(!local_err);

    /* 1. write 0x22 to S_LOCAL_DISK (IMG_SIZE / 2, IMG_SIZE) */
    blk = blk_by_name(S_LOCAL_DISK_ID);
    local_bs = blk_bs(blk);

    io_write(local_bs, 0x22, IMG_SIZE / 2, IMG_SIZE / 2, IMG_SIZE / 2, false);

    /* 2. replication will backup S_LOCAL_DISK to S_HIDDEN_DISK */
    io_read(top_bs, 0x11, IMG_SIZE / 2, IMG_SIZE / 2, 0, IMG_SIZE, false);

    replication_do_checkpoint_all(&local_err);
    g_assert(!local_err);

    /* 3. after checkpoint, read pattern 0x22 from S_LOCAL_DISK */
    io_read(top_bs, 0x22, IMG_SIZE / 2, IMG_SIZE / 2, 0, IMG_SIZE, false);

    /* unblock top_bs */
    replication_stop_all(failover, &local_err);
    g_assert(!local_err);

    teardown_secondary();
}

static void test_secondary_get_error(void)
{
    Error *local_err = NULL;
    bool failover = true;

    start_secondary();
    replication_start_all(REPLICATION_MODE_SECONDARY, &local_err);
    g_assert(!local_err);

    replication_get_error_all(&local_err);
    g_assert(!local_err);

    /* unblock top_bs */
    replication_stop_all(failover, &local_err);
    g_assert(!local_err);

    teardown_secondary();
}

int main(int argc, char **argv)
{
    int ret;
    qemu_init_main_loop(&error_fatal);
    bdrv_init();

    do {} while (g_main_context_iteration(NULL, false));
    g_test_init(&argc, &argv, NULL);

    prepare_imgs();

    /* Primary */
    g_test_add_func("/replication/primary/read",    test_primary_read);
    g_test_add_func("/replication/primary/write",   test_primary_write);
    g_test_add_func("/replication/primary/start",   test_primary_start);
    g_test_add_func("/replication/primary/stop",    test_primary_stop);
    g_test_add_func("/replication/primary/do_checkpoint",
                    test_primary_do_checkpoint);
    g_test_add_func("/replication/primary/get_error",
                    test_primary_get_error);

    /* Secondary */
    g_test_add_func("/replication/secondary/read",  test_secondary_read);
    g_test_add_func("/replication/secondary/write", test_secondary_write);
    g_test_add_func("/replication/secondary/start", test_secondary_start);
    g_test_add_func("/replication/secondary/stop",  test_secondary_stop);
    g_test_add_func("/replication/secondary/do_checkpoint",
                    test_secondary_do_checkpoint);
    g_test_add_func("/replication/secondary/get_error",
                    test_secondary_get_error);

    ret = g_test_run();

    cleanup_imgs();

    return ret;
}
