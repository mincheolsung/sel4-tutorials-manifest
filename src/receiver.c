
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

/* constants */
#define MSG_DATA 0x6161 //  arbitrary data to send

static struct fring *fring = NULL;
static struct aring *req_aring = NULL;
static struct aring *rsp_aring = NULL;
static void *data_buf = NULL;

static seL4_CPtr ep = 0;

static void init_rings(void *shared_mem) {
    fring = FRING(shared_mem);
    req_aring = REQ_ARING(shared_mem);
    rsp_aring = RSP_ARING(shared_mem);
    data_buf = DATA_BUF(shared_mem);
}

static void receiver(void) {
    unsigned long idx;
    unsigned long fails = 0;

    assert(ep != 0);
    assert(fring != NULL);

start_over:
    atomic_store(&req_aring->readers, 1);
    fails = 0;
again:
    while ((idx = lfring_dequeue((struct lfring *)req_aring->ring,
        RING_ORDER, false)) != LFRING_EMPTY) {
retry:
        fails = 0;

        printf("idx: %ld\n", idx);

        lfring_enqueue((struct lfring *) fring->ring,
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

    seL4_Wait(ep, NULL);

    goto start_over;
}

int main(int argc, char **argv) {
    printf("Receiver: hey hey hey\n");

    /* check arguments and get badged endpoint */
    ZF_LOGF_IF(argc < 2, "Missing arguments.\n");
    ep = (seL4_CPtr) atol(argv[0]);

    /* get shared memory address */
    void *shared_mem = (void *) atol(argv[1]);

    init_rings(shared_mem);

    receiver();

    return 0;
}
