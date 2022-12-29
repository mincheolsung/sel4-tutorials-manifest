
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

void init_rings(void *shared_mem) {
    fring = FRING(shared_mem);
    req_aring = REQ_ARING(shared_mem);
    rsp_aring = RSP_ARING(shared_mem);
    data_buf = DATA_BUF(shared_mem);
}

int main(int argc, char **argv) {
    seL4_MessageInfo_t tag;
    seL4_Word msg;

    printf("process_2: hey hey hey\n");

    /*
     * send a message to our parent, and wait for a reply
     */

    /* set the data to send. We send it in the first message register */
    tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, MSG_DATA);

    /* check arguments and get badged endpoint */
    ZF_LOGF_IF(argc < 2, "Missing arguments.\n");
    seL4_CPtr ep = (seL4_CPtr) atol(argv[0]);

    /* get shared memory address */
    void *shared_mem = (void *) atol(argv[1]);

    init_rings(shared_mem);

    unsigned long idx;
    while ((idx = lfring_dequeue((struct lfring *)req_aring->ring,
        RING_ORDER, false)) != LFRING_EMPTY) {

        printf("idx: %ld\n", idx);

        lfring_enqueue((struct lfring *) fring->ring,
            RING_ORDER, idx, false);
    }

    /* send and wait for a repliy */
    seL4_Call(ep, tag);

    /* check that we got the expected reply */
    assert(seL4_MessageInfo_get_length(tag) == 1);

    msg = seL4_GetMR(0);
    assert(msg == ~MSG_DATA);

    printf("process_2: got a reply: %#" PRIxPTR "\n", msg);

    return 0;
}
