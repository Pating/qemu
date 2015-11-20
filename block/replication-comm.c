/*
 * Replication Block filter
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * Author:
 *   Wen Congyang <wency@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "block/replication-comm.h"

static QLIST_HEAD(, BlockReplicationState) block_replication_states;

BlockReplicationState *block_replication_new(BlockDriverState *bs,
                                             BlockReplicationOps *ops)
{
    BlockReplicationState *brs;

    brs = g_new0(BlockReplicationState, 1);
    brs->bs = bs;
    brs->ops = ops;
    QLIST_INSERT_HEAD(&block_replication_states, brs, node);

    return brs;
}

void block_replication_remove(BlockReplicationState *brs)
{
    QLIST_REMOVE(brs, node);
    g_free(brs);
}

void block_replication_start_all(ReplicationMode mode, Error **errp)
{
    BlockReplicationState *brs, *next;
    QLIST_FOREACH_SAFE(brs, &block_replication_states, node, next) {
        if (brs->ops && brs->ops->start) {
            brs->ops->start(brs, mode, errp);
        }
    }
}

void block_replication_do_checkpoint_all(Error **errp)
{
    BlockReplicationState *brs, *next;
    QLIST_FOREACH_SAFE(brs, &block_replication_states, node, next) {
        if (brs->ops && brs->ops->checkpoint) {
            brs->ops->checkpoint(brs, errp);
        }
    }
}

void block_replication_stop_all(bool failover, Error **errp)
{
    BlockReplicationState *brs, *next;
    QLIST_FOREACH_SAFE(brs, &block_replication_states, node, next) {
        if (brs->ops && brs->ops->stop) {
            brs->ops->stop(brs, failover, errp);
        }
    }
}
