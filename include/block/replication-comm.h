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

#ifndef REPLICATION_H
#define REPLICATION_H

#include "qemu/queue.h"
#include "block/block_int.h"

typedef struct BlockReplicationOps BlockReplicationOps;
typedef struct BlockReplicationState BlockReplicationState;
typedef void (*Start)(BlockReplicationState *brs, ReplicationMode mode, Error **errp);
typedef void (*Stop)(BlockReplicationState *brs, bool failover, Error **errp);
typedef void (*Checkpoint)(BlockReplicationState *brs, Error **errp);

struct BlockReplicationState {
    BlockDriverState *bs;
    BlockReplicationOps *ops;
    QLIST_ENTRY(BlockReplicationState) node;
};

struct BlockReplicationOps{
    Start start;
    Stop stop;
    Checkpoint checkpoint;
};

BlockReplicationState *block_replication_new(BlockDriverState *bs,
                                             BlockReplicationOps *ops);

void block_replication_remove(BlockReplicationState *brs);

void block_replication_start_all(ReplicationMode mode, Error **errp);

void block_replication_do_checkpoint_all(Error **errp);

void block_replication_stop_all(bool failover, Error **errp);

#endif /* REPLICATION_H */
