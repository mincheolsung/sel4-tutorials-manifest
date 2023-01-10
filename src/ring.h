#ifndef __RING_H__
#define __RING_H__

#include <stddef.h>
#include "./include/wfring_cas2.h"
#include "./align.h"

#define WCQ_ORDER 10
#define DATA_SLOT_SIZE 32
#define BUFFER_SIZE  (1U << WCQ_ORDER)
#define PAGE_SIZE 4096

#define RING_PAGES ((WFRING_SIZE(WCQ_ORDER) + PAGE_SIZE - 1) / PAGE_SIZE)
#define BUFFER_PAGES   ((DATA_SLOT_SIZE * BUFFER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)
#define SHARED_PAGES (4*RING_PAGES + 2*BUFFER_PAGES)

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

#define  REQ_FRING_HD 0
#define  RSP_FRING_HD 1
#define  REQ_ARING_HD 2
#define  RSP_ARING_HD 3
#define  NUM_HANDLE 4

typedef struct _queue_t {
  char ring[WFRING_SIZE(WCQ_ORDER)];
} queue_t DOUBLE_CACHE_ALIGNED;

typedef struct _handle_t {
  struct wfring_state state;
} handle_t DOUBLE_CACHE_ALIGNED;

struct aring {
    _Alignas(LF_CACHE_BYTES) _Atomic(long) readers;
    queue_t q;
};

struct fring {
    queue_t q;
};

#endif
