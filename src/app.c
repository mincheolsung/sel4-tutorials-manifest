
/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * seL4 tutorial part 4: application to be run in a process
 */

#include <stdio.h>
#include <assert.h>

#include <sel4/sel4.h>
#include <sel4utils/process.h>

#include <utils/zf_log.h>
#include <sel4utils/sel4_zf_logif.h>

#include "../src/ring.h"

static struct fring *req_fring = NULL;
static struct fring *rsp_fring = NULL;
static struct aring *req_aring = NULL;
static struct aring *rsp_aring = NULL;
static void *req_data_buf = NULL;
static void *rsp_data_buf = NULL;

static seL4_CPtr receive_ep = 0;
static seL4_CPtr send_ep = 0;

static void init_rings(void *shared_mem) {
    req_fring = REQ_FRING(shared_mem);
    rsp_fring = RSP_FRING(shared_mem);
    req_aring = REQ_ARING(shared_mem);
    rsp_aring = RSP_ARING(shared_mem);
    req_data_buf = REQ_DATA_BUF(shared_mem);
    rsp_data_buf = RSP_DATA_BUF(shared_mem);
}

static void send_message(unsigned long m0, unsigned long m1, unsigned long m2, unsigned long m3) {
    unsigned long idx;
    unsigned long *slot;

    while ((idx = lfring_dequeue((struct lfring *) rsp_fring->ring, RING_ORDER, false)) == LFRING_EMPTY) {
        /* spin for available idx from free ring */
    }

    slot = (unsigned long *)((char *)rsp_data_buf + idx * DATA_SLOT_SIZE);
    slot[0] = m0;
    slot[1] = m1;
    slot[2] = m2;
    slot[3] = m3;

    lfring_enqueue((struct lfring *)rsp_aring->ring, RING_ORDER, idx, false);

    if (atomic_load(&rsp_aring->readers) <= 0) {
        seL4_Signal(send_ep);
    }
}

static void receiver(void) {
    unsigned long idx;
    unsigned long fails = 0;
    unsigned long *slot;

    assert(receive_ep != 0);
    assert(req_fring != NULL);

start_over:
    atomic_store(&req_aring->readers, 1);
    fails = 0;
again:
    while ((idx = lfring_dequeue((struct lfring *)req_aring->ring,
        RING_ORDER, false)) != LFRING_EMPTY) {
retry:
        fails = 0;

        slot = (unsigned long *)((char *)req_data_buf + idx * DATA_SLOT_SIZE);
        send_message(slot[0], slot[1], slot[2], slot[3]);

        lfring_enqueue((struct lfring *) req_fring->ring,
            RING_ORDER, idx, false);
    }
    if (++fails < 1024) {
        goto again;
    }
    atomic_store(&req_aring->readers, -1);

    idx = lfring_dequeue((struct lfring *)req_aring->ring,
        RING_ORDER, false);
    if (idx != LFRING_EMPTY) {
        atomic_store(&req_aring->readers, 1);
        goto retry;
    }

    seL4_Wait(receive_ep, NULL);

    goto start_over;
}

int main(int argc, char **argv) {
    printf("App: hey hey hey\n");

    /* check arguments and get badged endpoint */
    ZF_LOGF_IF(argc < 3, "Missing arguments.\n");
    receive_ep = (seL4_CPtr) atol(argv[0]);
    send_ep = (seL4_CPtr) atol(argv[1]);

    /* get shared memory address */
    void *shared_mem = (void *) atol(argv[2]);

    init_rings(shared_mem);

    receiver();

    return 0;
}
