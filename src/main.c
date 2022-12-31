
/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <autoconf.h>

#include <stdio.h>
#include <assert.h>
#include <unistd.h>

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

#include <sel4runtime.h>
#include <sel4runtime/gen_config.h>


#include "../src/ring.h"

/* constants */
#define EP_BADGE1 0x61 // arbitrary (but unique) number for a badge
#define EP_BADGE2 0x62 // arbitrary (but unique) number for a badge

#define IPCBUF_FRAME_SIZE_BITS 12 // use a 4K frame for the IPC buffer
#define IPCBUF_VADDR 0x7000000 // arbitrary (but free) address for IPC buffer

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

/* tls region for the new thread */
static char tls_region[CONFIG_SEL4RUNTIME_STATIC_TLS] = {};

static struct fring *req_fring = NULL;
static struct fring *rsp_fring = NULL;
static struct aring *req_aring = NULL;
static struct aring *rsp_aring = NULL;
static void *req_data_buf = NULL;
static void *rsp_data_buf = NULL;

static cspacepath_t sender_ep_cap_path;
static cspacepath_t receiver_ep_cap_path;

static void init_rings(void *shared_mem) {
    printf("Main: init_rings SHARED_PAGES: %ld\n", SHARED_PAGES);

    req_fring = REQ_FRING(shared_mem);
    rsp_fring = RSP_FRING(shared_mem);
    req_aring = REQ_ARING(shared_mem);
    rsp_aring = RSP_ARING(shared_mem);
    req_data_buf = REQ_DATA_BUF(shared_mem);
    rsp_data_buf = RSP_DATA_BUF(shared_mem);

    /* init ring */
    lfring_init_fill((struct lfring *)req_fring->ring, 0, BUFFER_SIZE, RING_ORDER);
    lfring_init_fill((struct lfring *)rsp_fring->ring, 0, BUFFER_SIZE, RING_ORDER);
    lfring_init_empty((struct lfring *)req_aring->ring, RING_ORDER);
    lfring_init_empty((struct lfring *)rsp_aring->ring, RING_ORDER);

    atomic_init(&req_fring->readers, 1);
    atomic_init(&rsp_fring->readers, 1);
    atomic_init(&req_aring->readers, 0);
    atomic_init(&rsp_aring->readers, 0);
}

static void send(int iter) {
    unsigned long idx;
    unsigned long cnt = 0;
    unsigned long *slot;
again:
    while ((idx = lfring_dequeue((struct lfring *) req_fring->ring, RING_ORDER, false)) == LFRING_EMPTY) {
        /* spin for available idx from free ring */
    }

    slot = (unsigned long *)req_data_buf + idx;
    slot[0] = cnt;

    lfring_enqueue((struct lfring *)req_aring->ring, RING_ORDER, idx, false);

    if (atomic_load(&req_aring->readers) <= 0) {
        seL4_Signal(sender_ep_cap_path.capPtr);
    }

    if (++cnt >= iter) {
        return;
    }

    goto again;
}

static void receiver(void) {
    unsigned long idx;
    unsigned long fails = 0;
    unsigned long *slot;

    assert(receiver_ep_cap_path.capPtr != 0);
    assert(req_fring != NULL);

start_over:
    atomic_store(&rsp_aring->readers, 1);
    fails = 0;
again:
    while ((idx = lfring_dequeue((struct lfring *)rsp_aring->ring,
        RING_ORDER, false)) != LFRING_EMPTY) {
retry:
        fails = 0;

        slot = (unsigned long *)rsp_data_buf + idx;
        //printf("%lu\n", slot[0]);

        lfring_enqueue((struct lfring *) rsp_fring->ring,
            RING_ORDER, idx, false);
    }
    if (++fails < 1024) {
        goto again;
    }
    atomic_store(&rsp_aring->readers, -1);

    idx = lfring_dequeue((struct lfring *)rsp_aring->ring,
        RING_ORDER, false);
    if (idx != LFRING_EMPTY) {
        atomic_store(&rsp_aring->readers, 1);
        goto retry;
    }

    seL4_Wait(receiver_ep_cap_path.capPtr, NULL);

    goto start_over;
}

static void create_receiver_thread(void) {
    UNUSED int error = 0;

    /* get cspace root cnode */
    seL4_CPtr cspace_cap = simple_get_cnode(&simple);

    /* get vspace root page directory */
    seL4_CPtr pd_cap = simple_get_pd(&simple);

    /* create a new TCB for receiving thread */
    vka_object_t tcb_object = {0};
    error = vka_alloc_tcb(&vka, &tcb_object);
    assert(error == 0);

    /* create and map an ipc buffer */

    /* get a frame cap for the ipc buffer */
    vka_object_t ipc_frame_object;
    error = vka_alloc_frame(&vka, IPCBUF_FRAME_SIZE_BITS, &ipc_frame_object);
    assert(error == 0);

    /* map the frame into the vspace at ipc_buffer_vaddr */
    seL4_Word ipc_buffer_vaddr = IPCBUF_VADDR;

    /* try to map the frame the first time */
    error = seL4_ARCH_Page_Map(ipc_frame_object.cptr, pd_cap, ipc_buffer_vaddr, seL4_AllRights,
                               seL4_ARCH_Default_VMAttributes);
    if (error != 0) {
        /* create a page table */
        vka_object_t pt_object;
        error = vka_alloc_page_table(&vka, &pt_object);
        assert(error == 0);

        /* map the page table */
        error = seL4_ARCH_PageTable_Map(pt_object.cptr, pd_cap, ipc_buffer_vaddr,
                                        seL4_ARCH_Default_VMAttributes);
        assert(error == 0);

        /* map the frame in */
        error = seL4_ARCH_Page_Map(ipc_frame_object.cptr, pd_cap, ipc_buffer_vaddr, seL4_AllRights,
                                   seL4_ARCH_Default_VMAttributes);
        assert(error == 0);
    }

    /* init the new TCB */
    error = seL4_TCB_Configure(tcb_object.cptr, seL4_CapNull, cspace_cap, seL4_NilData, pd_cap,
                               seL4_NilData, ipc_buffer_vaddr, ipc_frame_object.cptr);
    assert(error == 0);

    /* set the priority of the new thread to be equal to our priority */
    error = seL4_TCB_SetPriority(tcb_object.cptr, simple_get_tcb(&simple), 255);
    assert(error == 0);

    NAME_THREAD(tcb_object.cptr, "main: receiver");

    /* set start up registers for the new thread */
    UNUSED seL4_UserContext regs = {0};
    size_t regs_size = sizeof(seL4_UserContext) / sizeof(seL4_Word);

    /* set instruction pointer where the thread shoud start running */
    sel4utils_set_instruction_pointer(&regs, (seL4_Word)receiver);

    /* check that stack is aligned correctly */
    const int stack_alignment_requirement = sizeof(seL4_Word) * 2;
    uintptr_t thread_2_stack_top = (uintptr_t)thread_2_stack + sizeof(thread_2_stack);
    assert(thread_2_stack_top % (stack_alignment_requirement) == 0);

    /* set stack pointer for the new thread */
    sel4utils_set_stack_pointer(&regs, thread_2_stack_top);

    /* actually write the TCB registers */
    error = seL4_TCB_WriteRegisters(tcb_object.cptr, 0, 0, regs_size, &regs);
    assert(error == 0);

    /* create a thread local storage (TLS) region for the new thread to store the
      ipc buffer pointer */
    uintptr_t tls = sel4runtime_write_tls_image(tls_region);
    seL4_IPCBuffer *ipcbuf = (seL4_IPCBuffer*)ipc_buffer_vaddr;
    error = sel4runtime_set_tls_variable(tls, __sel4_ipc_buffer, ipcbuf);
    assert(error == 0);

    /* set the TLS base of the new thread */
    error = seL4_TCB_SetTLSBase(tcb_object.cptr, tls);
    assert(error == 0);

    /* start the new thread running */
    error = seL4_TCB_Resume(tcb_object.cptr);
    assert(error == 0);
}

void static create_process(void) {
    UNUSED int error = 0;

    /* use sel4utils to make a new process */
    sel4utils_process_t new_process;
    sel4utils_process_config_t config = process_config_default_simple(&simple, APP_IMAGE_NAME, APP_PRIORITY);
    config = process_config_auth(config, simple_get_tcb(&simple));
    config = process_config_priority(config, seL4_MaxPrio);
    error = sel4utils_configure_process_custom(&new_process, &vka, &vspace, config);
    assert(error == 0);

    /* give the new process's thread a name */
    NAME_THREAD(new_process.thread.tcb.cptr, "app");

    /* create an endpoint for sender */
    vka_object_t sender_ep_object = {0};
    error = vka_alloc_endpoint(&vka, &sender_ep_object);
    assert(error == 0);

    /* create an endpoint for receiver */
    vka_object_t receiver_ep_object = {0};
    error = vka_alloc_endpoint(&vka, &receiver_ep_object);
    assert(error == 0);

    /*
     * make a badged endpoint in the new process's cspace.  This copy
     * will be used to send an IPC to the original cap
     */

    /* make a cspacepath for the new sender's endpoint cap */
    seL4_CPtr sender_ep_cap = 0;
    vka_cspace_make_path(&vka, sender_ep_object.cptr, &sender_ep_cap_path);

    /* copy the endpoint cap and add a badge to the new cap */
    sender_ep_cap = sel4utils_mint_cap_to_process(&new_process, sender_ep_cap_path, seL4_AllRights, EP_BADGE1);
    assert(sender_ep_cap != 0);

    /* make a cspacepath for the new receiver's endpoint cap */
    seL4_CPtr receiver_ep_cap = 0;
    vka_cspace_make_path(&vka, receiver_ep_object.cptr, &receiver_ep_cap_path);

    /* copy the endpoint cap and add a badge to the new cap */
    receiver_ep_cap = sel4utils_mint_cap_to_process(&new_process, receiver_ep_cap_path, seL4_AllRights, EP_BADGE2);
    assert(receiver_ep_cap != 0);

    /* set up shared memory */
    void *shared_mem = vspace_new_pages(&vspace, seL4_AllRights, SHARED_PAGES, seL4_PageBits);
    assert(shared_mem != NULL);

    void *app_shared_mem = vspace_share_mem(&vspace, &new_process.vspace, shared_mem, SHARED_PAGES,
                                            seL4_PageBits, seL4_AllRights, true);
    assert(app_shared_mem != NULL);

    /* init ring buffer */
    init_rings(shared_mem);

    /* spawn the process */
    seL4_Word argc = 3;
    char string_args[argc][WORD_STRING_SIZE];
    char* argv[argc];
    int resume = 1;
    sel4utils_create_word_args(string_args, argv, argc, sender_ep_cap, receiver_ep_cap, app_shared_mem);

    error = sel4utils_spawn_process_v(&new_process, &vka, &vspace, argc, (char**) &argv, resume);
    assert(error == 0);
}

int main(void) {
    UNUSED int error = 0;

    /* get boot info */
    info = platsupport_get_bootinfo();
    assert(info != NULL);

    /* Set up logging and give us a name: useful for debugging if the thread faults */
    zf_log_set_tag_prefix("main:");
    NAME_THREAD(seL4_CapInitThreadTCB, "main");

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
    assert(virtual_reservation.res != NULL);
    bootstrap_configure_virtual_pool(allocman, vaddr,
                                     ALLOCATOR_VIRTUAL_POOL_SIZE, simple_get_pd(&simple));

    /*
     * now create a process
     */
    create_process();

    /*
     * now create a receiver thread
     */
    create_receiver_thread();

    /* we are done, say hello */
    printf("Main: hello world\n");

    //seL4_DebugDumpScheduler();
    send(100000);

    return 0;
}
