#ifndef __RING_H__
#define __RING_H__

#include "./include/lfring.h"

#define RING_ORDER   10
#define BUFFER_ORDER 10
#define DATA_SLOT_SIZE 32

#define PAGE_SIZE 4096

#define RING_SIZE    (1U << RING_ORDER)
#define BUFFER_SIZE  (1U << BUFFER_ORDER)
#define RING_PAGES ((LFRING_SIZE(RING_ORDER) + PAGE_SIZE - 1) / PAGE_SIZE)
#define BUFFER_PAGES   ((DATA_SLOT_SIZE * BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)

#define SHARED_PAGES (4*RING_PAGES + 2*BUFFER_PAGES)

/* ring buffer structures */
#define REQ_FRING(shared_mem)       \
        ((struct fring *) ((char *) shared_mem + 0 * RING_PAGES * PAGE_SIZE))

#define RSP_FRING(shared_mem)       \
        ((struct fring *) ((char *) shared_mem + 1 * RING_PAGES * PAGE_SIZE))

#define REQ_ARING(shared_mem)       \
        ((struct aring *) ((char *) shared_mem + 2 * RING_PAGES * PAGE_SIZE))

#define RSP_ARING(shared_mem)   \
        ((struct aring *) ((char *) shared_mem + 3 * RING_PAGES * PAGE_SIZE))

#define REQ_DATA_BUF(shared_mem) \
        ((char *) shared_mem + 4 * RING_PAGES * PAGE_SIZE)

#define RSP_DATA_BUF(shared_mem) \
        ((char *) shared_mem + (4 * RING_PAGES + BUFFER_PAGES) * PAGE_SIZE)

struct aring {
    _Alignas(LF_CACHE_BYTES) _Atomic(long) readers;
    _Alignas(LFRING_ALIGN) char ring[0];
};

struct fring {
    _Alignas(LF_CACHE_BYTES) _Atomic(long) readers;
    _Alignas(LFRING_ALIGN) char ring[0];
};

#endif
