
/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <autoconf.h>

#include <stdio.h>
#include <assert.h>

#include <sel4/sel4.h>

#include <simple/simple.h>
#include <simple-default/simple-default.h>

#include <vka/object.h>

#include <allocman/allocman.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>

#include <vspace/vspace.h>

#include <sel4utils/vspace.h>
#include <sel4utils/mapping.h>
#include <sel4utils/process.h>

#include <utils/arith.h>
#include <utils/zf_log.h>
#include <sel4utils/sel4_zf_logif.h>

#include <sel4platsupport/bootinfo.h>

#include "../src/ring.h"

/* constants */
#define EP_BADGE 0x61 // arbitrary (but unique) number for a badge
#define MSG_DATA 0x6161 // arbitrary data to send

#define APP_PRIORITY seL4_MaxPrio
#define APP_IMAGE_NAME "app"

/* global environment variables */
seL4_BootInfo *info;
simple_t simple;
vka_t vka;
allocman_t *allocman;
vspace_t vspace;

/* static memory for the allocator to bootstrap with */
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 10)
UNUSED static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

/* dimensions of virtual memory for the allocator to use */
#define ALLOCATOR_VIRTUAL_POOL_SIZE (BIT(seL4_PageBits) * 100)

/* static memory for virtual memory bootstrapping */
UNUSED static sel4utils_alloc_data_t data;

/* stack for the new thread */
#define THREAD_2_STACK_SIZE 4096
UNUSED static int thread_2_stack[THREAD_2_STACK_SIZE];

static struct fring *fring = NULL;
static struct aring *req_aring = NULL;
static struct aring *rsp_aring = NULL;
static void *data_buf = NULL;

static void init_rings(void *shared_mem) {
    printf("Main: init_rings SHARED_PAGES: %ld\n", SHARED_PAGES);

    fring = FRING(shared_mem);
    req_aring = REQ_ARING(shared_mem);
    rsp_aring = RSP_ARING(shared_mem);
    data_buf = DATA_BUF(shared_mem);

    /* init ring */
    lfring_init_fill((struct lfring *)fring->ring, 0, BUFFER_SIZE, RING_ORDER);
    lfring_init_empty((struct lfring *)req_aring->ring, RING_ORDER);
    lfring_init_empty((struct lfring *)rsp_aring->ring, RING_ORDER);

    atomic_init(&fring->readers, 1);
    atomic_init(&req_aring->readers, 0);
    atomic_init(&rsp_aring->readers, 0);
}

int main(void) {
    UNUSED int error = 0;

    /* get boot info */
    info = platsupport_get_bootinfo();
    ZF_LOGF_IF(info == NULL, "Failed to get bootinfo.");

    /* Set up logging and give us a name: useful for debugging if the thread faults */
    zf_log_set_tag_prefix("dynamic-3:");
    NAME_THREAD(seL4_CapInitThreadTCB, "dynamic-3");

    /* init simple */
    simple_default_init_bootinfo(&simple, info);

    /* print out bootinfo and other info about simple */
    //simple_print(&simple);

    /* create an allocator */
    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE,
                                            allocator_mem_pool);
    assert(allocman);

    /* create a vka (interface for interacting with the underlying allocator) */
    allocman_make_vka(&vka, allocman);

    /* create a vspace object to manage our vspace */
    error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(&vspace, &data, simple_get_pd(&simple), &vka, info);
    assert(error == 0);

    /* fill the allocator with virtual memory */
    void *vaddr;
    UNUSED reservation_t virtual_reservation;
    virtual_reservation = vspace_reserve_range(&vspace,
                                               ALLOCATOR_VIRTUAL_POOL_SIZE, seL4_AllRights, 1, &vaddr);
    ZF_LOGF_IF(virtual_reservation.res == NULL, "Failed to reserve a chunk of memory.\n");
    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&simple));

    /* use sel4utils to make a new process */
    sel4utils_process_t new_process;
    sel4utils_process_config_t config = process_config_default_simple(&simple, APP_IMAGE_NAME, APP_PRIORITY);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_priority(config, seL4_MaxPrio);
    error = sel4utils_configure_process_custom(&new_process, &vka, &vspace, config);
    assert(error == 0);

    /* give the new process's thread a name */
    NAME_THREAD(new_process.thread.tcb.cptr, "dynamic-3: process_2");

    /* create an endpoint */
    vka_object_t ep_object = {0};
    error = vka_alloc_endpoint(&vka, &ep_object);
    assert(error == 0);

    /*
     * make a badged endpoint in the new process's cspace.  This copy
     * will be used to send an IPC to the original cap
     */

    /* make a cspacepath for the new endpoint cap */
    cspacepath_t ep_cap_path;
    seL4_CPtr new_ep_cap = 0;
    vka_cspace_make_path(&vka, ep_object.cptr, &ep_cap_path);

    /* copy the endpoint cap and add a badge to the new cap */
    new_ep_cap = sel4utils_mint_cap_to_process(&new_process, ep_cap_path, seL4_AllRights, EP_BADGE);
    assert(new_ep_cap != 0);

    //printf("NEW CAP SLOT: %" PRIxPTR ".\n", ep_cap_path.capPtr);

    /* set up shared memory */
    void *shared_mem = vspace_new_pages(&vspace, seL4_AllRights, SHARED_PAGES, seL4_PageBits);
    assert(shared_mem);

    void *app_shared_mem = vspace_share_mem(&vspace, &new_process.vspace, shared_mem, SHARED_PAGES,
                                            seL4_PageBits, seL4_AllRights, true);
    assert(app_shared_mem);

    /* init ring buffer */
    init_rings(shared_mem);

    for (int i = 0; i < 10; i++) {
        unsigned long idx = lfring_dequeue((struct lfring *) fring->ring, RING_ORDER, false);
        printf("enqueue %ld\n", idx);
        lfring_enqueue((struct lfring *)req_aring->ring, RING_ORDER, idx, false);
    }

    /* spawn the process */
    seL4_Word argc = 2;
    char string_args[argc][WORD_STRING_SIZE];
    char* argv[argc];
    sel4utils_create_word_args(string_args, argv, argc, new_ep_cap, app_shared_mem);

    error = sel4utils_spawn_process_v(&new_process, &vka, &vspace, argc, (char**) &argv, 1);
    assert(error == 0);


    /* we are done, say hello */
    printf("main: hello world\n");

    /*
     * now wait for a message from the new process, then send a reply back
     */
    seL4_Word sender_badge = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);

    /* wait for a message */
    tag = seL4_Recv(ep_cap_path.capPtr, &sender_badge);

    /* make sure it is what we expected */
    assert(sender_badge == EP_BADGE);
    assert(seL4_MessageInfo_get_length(tag) == 1);

    /* get the message stored in the first message register */
    seL4_Word msg = seL4_GetMR(0);
    printf("main: got a message %#" PRIxPTR " from %#" PRIxPTR "\n", msg, sender_badge);

    /* modify the message */
    seL4_SetMR(0, ~msg);

    /* send the modified message back */
    seL4_ReplyRecv(ep_cap_path.capPtr, tag, &sender_badge);

    return 0;
}
