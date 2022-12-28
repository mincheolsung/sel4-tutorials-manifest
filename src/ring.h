#ifndef __RING_H__
#define __RING_H__

#include "./include/lfring.h"

#define RING_ORDER   4
#define BUFFER_ORDER 4
#define DATA_SLOT_SIZE 64

#define PAGE_SIZE 4096

#define RING_SIZE    (1U << RING_ORDER)
#define DATA_BUFFER_SIZE  (1U << BUFFER_ORDER)
#define RING_PAGES ((LFRING_SIZE(RING_ORDER) + PAGE_SIZE - 1) / PAGE_SIZE)
#define DATA_BUFFER_PAGES   ((DATA_SLOT_SIZE * DATA_BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)

#define SHARED_PAGES (3*RING_PAGES + DATA_BUFFER_PAGES)

/* ring buffer structures */
#define FRING(shared_mem)       \
        ((struct fring *) ((char *) shared_mem + 0 * PAGE_SIZE * PAGE_SIZE))

#define REQ_ARING(shared_mem)       \
        ((struct aring *) ((char *) shared_mem + 1 * RING_PAGES * PAGE_SIZE))

#define RSP_ARING(shared_mem)   \
        ((struct aring *) ((char *) shared_mem + 2 * RING_PAGES * PAGE_SIZE))

#define DATA_BUF(shared_mem) \
        ((char *) shared_mem + 3 * RING_PAGES * PAGE_SIZE)

struct aring {
    _Alignas(LF_CACHE_BYTES) _Atomic(long) readers;
    _Alignas(LFRING_ALIGN) char ring[0];
};

struct fring {
    _Alignas(LF_CACHE_BYTES) _Atomic(long) readers;
    _Alignas(LFRING_ALIGN) char ring[0];
};
 #endif
