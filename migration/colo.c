/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2016 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2016 FUJITSU LIMITED
 * Copyright (c) 2016 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "migration/colo.h"
#include "trace.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "migration/failover.h"

/* colo buffer */
#define COLO_BUFFER_BASE_SIZE (4 * 1024 * 1024)

bool colo_supported(void)
{
    return true;
}

bool migration_in_colo_state(void)
{
    MigrationState *s = migrate_get_current();

    return (s->state == MIGRATION_STATUS_COLO);
}

bool migration_incoming_in_colo_state(void)
{
    MigrationIncomingState *mis = migration_incoming_get_current();

    return mis && (mis->state == MIGRATION_STATUS_COLO);
}

static void colo_send_message(QEMUFile *f, COLOMessage msg,
                              Error **errp)
{
    int ret;

    if (msg >= COLO_MESSAGE__MAX) {
        error_setg(errp, "%s: Invalid message", __func__);
        return;
    }
    qemu_put_be32(f, msg);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Can't send COLO message");
    }
    trace_colo_send_message(COLOMessage_lookup[msg]);
}

static void colo_send_message_value(QEMUFile *f, COLOMessage msg,
                                    uint64_t value, Error **errp)
{
    Error *local_err = NULL;
    int ret;

    colo_send_message(f, msg, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    qemu_put_be64(f, value);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Failed to send value for message:%s",
                         COLOMessage_lookup[msg]);
    }
}

static COLOMessage colo_receive_message(QEMUFile *f, Error **errp)
{
    COLOMessage msg;
    int ret;

    msg = qemu_get_be32(f);
    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Can't receive COLO message");
        return msg;
    }
    if (msg >= COLO_MESSAGE__MAX) {
        error_setg(errp, "%s: Invalid message", __func__);
        return msg;
    }
    trace_colo_receive_message(COLOMessage_lookup[msg]);
    return msg;
}

static void colo_receive_check_message(QEMUFile *f, COLOMessage expect_msg,
                                       Error **errp)
{
    COLOMessage msg;
    Error *local_err = NULL;

    msg = colo_receive_message(f, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (msg != expect_msg) {
        error_setg(errp, "Unexpected COLO message %d, expected %d",
                          msg, expect_msg);
    }
}

static uint64_t colo_receive_message_value(QEMUFile *f, uint32_t expect_msg,
                                           Error **errp)
{
    Error *local_err = NULL;
    uint64_t value;
    int ret;

    colo_receive_check_message(f, expect_msg, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return 0;
    }

    value = qemu_get_be64(f);
    ret = qemu_file_get_error(f);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Failed to get value for COLO message: %s",
                         COLOMessage_lookup[expect_msg]);
    }
    return value;
}

static int colo_do_checkpoint_transaction(MigrationState *s,
                                          QEMUSizedBuffer *buffer)
{
    QEMUFile *trans = NULL;
    size_t size;
    Error *local_err = NULL;
    int ret = -1;

    colo_send_message(s->to_dst_file, COLO_MESSAGE_CHECKPOINT_REQUEST,
                      &local_err);
    if (local_err) {
        goto out;
    }

    colo_receive_check_message(s->rp_state.from_dst_file,
                    COLO_MESSAGE_CHECKPOINT_REPLY, &local_err);
    if (local_err) {
        goto out;
    }
    /* Reset colo buffer and open it for write */
    qsb_set_length(buffer, 0);
    trans = qemu_bufopen("w", buffer);
    if (!trans) {
        error_report("Open colo buffer for write failed");
        goto out;
    }

    qemu_mutex_lock_iothread();
    vm_stop_force_state(RUN_STATE_COLO);
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("run", "stop");

    /* Disable block migration */
    s->params.blk = 0;
    s->params.shared = 0;
    qemu_savevm_state_header(trans);
    qemu_savevm_state_begin(trans, &s->params);
    qemu_mutex_lock_iothread();
    qemu_savevm_state_complete_precopy(trans, false);
    qemu_mutex_unlock_iothread();

    qemu_fflush(trans);

    colo_send_message(s->to_dst_file, COLO_MESSAGE_VMSTATE_SEND, &local_err);
    if (local_err) {
        goto out;
    }
    /* we send the total size of the vmstate first */
    size = qsb_get_length(buffer);
    colo_send_message_value(s->to_dst_file, COLO_MESSAGE_VMSTATE_SIZE,
                            size, &local_err);
    if (local_err) {
        goto out;
    }

    qsb_put_buffer(s->to_dst_file, buffer, size);
    qemu_fflush(s->to_dst_file);
    ret = qemu_file_get_error(s->to_dst_file);
    if (ret < 0) {
        goto out;
    }

    colo_receive_check_message(s->rp_state.from_dst_file,
                       COLO_MESSAGE_VMSTATE_RECEIVED, &local_err);
    if (local_err) {
        goto out;
    }

    colo_receive_check_message(s->rp_state.from_dst_file,
                       COLO_MESSAGE_VMSTATE_LOADED, &local_err);
    if (local_err) {
        goto out;
    }

    ret = 0;
    /* Resume primary guest */
    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

out:
    if (local_err) {
        error_report_err(local_err);
    }
    if (trans) {
        qemu_fclose(trans);
    }
    return ret;
}

static void colo_process_checkpoint(MigrationState *s)
{
    QEMUSizedBuffer *buffer = NULL;
    int64_t current_time, checkpoint_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    Error *local_err = NULL;
    int ret;

    failover_init_state();

    s->rp_state.from_dst_file = qemu_file_get_return_path(s->to_dst_file);
    if (!s->rp_state.from_dst_file) {
        error_report("Open QEMUFile from_dst_file failed");
        goto out;
    }

    /*
     * Wait for Secondary finish loading vm states and enter COLO
     * restore.
     */
    colo_receive_check_message(s->rp_state.from_dst_file,
                       COLO_MESSAGE_CHECKPOINT_READY, &local_err);
    if (local_err) {
        goto out;
    }

    buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

    while (s->state == MIGRATION_STATUS_COLO) {
        current_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
        if (current_time - checkpoint_time <
            s->parameters[MIGRATION_PARAMETER_X_CHECKPOINT_DELAY]) {
            int64_t delay_ms;

            delay_ms = s->parameters[MIGRATION_PARAMETER_X_CHECKPOINT_DELAY] -
                       (current_time - checkpoint_time);
            g_usleep(delay_ms * 1000);
        }
        /* start a colo checkpoint */
        ret = colo_do_checkpoint_transaction(s, buffer);
        if (ret < 0) {
            goto out;
        }
        checkpoint_time = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    }

out:
    /* Throw the unreported error message after exited from loop */
    if (local_err) {
        error_report_err(local_err);
    }
    migrate_set_state(&s->state, MIGRATION_STATUS_COLO,
                      MIGRATION_STATUS_COMPLETED);

    qsb_free(buffer);
    buffer = NULL;

    if (s->rp_state.from_dst_file) {
        qemu_fclose(s->rp_state.from_dst_file);
    }
}

void migrate_start_colo_process(MigrationState *s)
{
    qemu_mutex_unlock_iothread();
    migrate_set_state(&s->state, MIGRATION_STATUS_ACTIVE,
                      MIGRATION_STATUS_COLO);
    colo_process_checkpoint(s);
    qemu_mutex_lock_iothread();
}

static void colo_wait_handle_message(QEMUFile *f, int *checkpoint_request,
                                     Error **errp)
{
    COLOMessage msg;
    Error *local_err = NULL;

    msg = colo_receive_message(f, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    switch (msg) {
    case COLO_MESSAGE_CHECKPOINT_REQUEST:
        *checkpoint_request = 1;
        break;
    default:
        *checkpoint_request = 0;
        error_setg(errp, "Got unknown COLO message: %d", msg);
        break;
    }
}

void *colo_process_incoming_thread(void *opaque)
{
    MigrationIncomingState *mis = opaque;
    QEMUFile *fb = NULL;
    QEMUSizedBuffer *buffer = NULL; /* Cache incoming device state */
    uint64_t total_size;
    uint64_t value;
    Error *local_err = NULL;
    int ret;

    migrate_set_state(&mis->state, MIGRATION_STATUS_ACTIVE,
                      MIGRATION_STATUS_COLO);

    failover_init_state();

    mis->to_src_file = qemu_file_get_return_path(mis->from_src_file);
    if (!mis->to_src_file) {
        error_report("colo incoming thread: Open QEMUFile to_src_file failed");
        goto out;
    }
    /* Note: We set the fd to unblocked in migration incoming coroutine,
    *  But here we are in the colo incoming thread, so it is ok to set the
    *  fd back to blocked.
    */
    qemu_file_set_blocking(mis->from_src_file, true);

    ret = colo_init_ram_cache();
    if (ret < 0) {
        error_report("Failed to initialize ram cache");
        goto out;
    }

    buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    colo_send_message(mis->to_src_file, COLO_MESSAGE_CHECKPOINT_READY,
                      &local_err);
    if (local_err) {
        goto out;
    }

    while (mis->state == MIGRATION_STATUS_COLO) {
        int request;

        colo_wait_handle_message(mis->from_src_file, &request, &local_err);
        if (local_err) {
            goto out;
        }
        assert(request);
        /* FIXME: This is unnecessary for periodic checkpoint mode */
        colo_send_message(mis->to_src_file, COLO_MESSAGE_CHECKPOINT_REPLY,
                     &local_err);
        if (local_err) {
            goto out;
        }

        colo_receive_check_message(mis->from_src_file,
                           COLO_MESSAGE_VMSTATE_SEND, &local_err);
        if (local_err) {
            goto out;
        }

        /* read the VM state total size first */
        value = colo_receive_message_value(mis->from_src_file,
                                 COLO_MESSAGE_VMSTATE_SIZE, &local_err);
        if (local_err) {
            goto out;
        }

        /* read vm device state into colo buffer */
        total_size = qsb_fill_buffer(buffer, mis->from_src_file, value);
        if (total_size != value) {
            error_report("Got %lu VMState data, less than expected %lu",
                         total_size, value);
            ret = -EINVAL;
            goto out;
        }

        colo_send_message(mis->to_src_file, COLO_MESSAGE_VMSTATE_RECEIVED,
                     &local_err);
        if (local_err) {
            goto out;
        }

        /* open colo buffer for read */
        fb = qemu_bufopen("r", buffer);
        if (!fb) {
            error_report("Can't open colo buffer for read");
            goto out;
        }

        qemu_mutex_lock_iothread();
        qemu_system_reset(VMRESET_SILENT);
        if (qemu_loadvm_state(fb) < 0) {
            error_report("COLO: loadvm failed");
            qemu_mutex_unlock_iothread();
            goto out;
        }
        qemu_mutex_unlock_iothread();

        colo_send_message(mis->to_src_file, COLO_MESSAGE_VMSTATE_LOADED,
                     &local_err);
        if (local_err) {
            goto out;
        }

        qemu_fclose(fb);
        fb = NULL;
    }

out:
    /* Throw the unreported error message after exited from loop */
    if (local_err) {
        error_report_err(local_err);
    }

    if (fb) {
        qemu_fclose(fb);
    }
    qsb_free(buffer);

    qemu_mutex_lock_iothread();
    colo_release_ram_cache();
    qemu_mutex_unlock_iothread();

    if (mis->to_src_file) {
        qemu_fclose(mis->to_src_file);
    }
    migration_incoming_exit_colo();

    return NULL;
}
