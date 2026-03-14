#include <include/kmalloc.h>
#include "vmalloc.h"

#include <include/printk.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* Each pool chunk requested from vmalloc.  4 MiB gives plenty of room for
 * typical kernel use while keeping page-table overhead low.              */
#define KMALLOC_POOL_SIZE   (4ULL * 1024ULL * 1024ULL)

/* All returned pointers are aligned to this boundary.                    */
#define KMALLOC_ALIGN       16UL

/* Minimum leftover size (including a new header) that justifies splitting
 * a free block into two.  Stops the heap filling up with useless slivers. */
#define KMALLOC_MIN_SPLIT   (sizeof(kmalloc_block_t) + KMALLOC_ALIGN)

/* Magic values stored in every block header.  Lets kfree() catch the most
 * common corruption / double-free mistakes immediately.                  */
#define KMALLOC_MAGIC_FREE  0xFEEBEEFDUL
#define KMALLOC_MAGIC_USED  0xDEADBEEFUL

/*
 * Block header
 *
 * Lives immediately BEFORE the pointer returned to the caller.
 *
 * Physical (address-order) chain:
 *   block → next_phys → …  (used for coalescing on free)
 *
 * Free-list (doubly-linked, unordered):
 *   block ↔ next_free / prev_free  (only valid while block->free == 1)
 *
 * The struct is padded to 48 bytes — a multiple of KMALLOC_ALIGN (16) —
 * so that, given a 16-byte-aligned block start, (block + 1) is also
 * 16-byte aligned. */


typedef struct kmalloc_block {
    uint32_t magic;                     /*  4 bytes — integrity tag        */
    uint32_t free;                      /*  4 bytes — 1 = free, 0 = used   */
    size_t   size;                      /*  8 bytes — usable payload bytes  */
    struct kmalloc_block *next_phys;    /*  8 bytes — next block in memory  */
    struct kmalloc_block *next_free;    /*  8 bytes — next block in freelist*/
    struct kmalloc_block *prev_free;    /*  8 bytes — prev block in freelist*/
    uint8_t  _pad[8];                   /*  8 bytes — pad struct to 48 B    */
} kmalloc_block_t;

/* Sanity-checked at compile time below. */
_Static_assert(sizeof(kmalloc_block_t) == 48,
               "kmalloc_block_t must be 48 bytes for 16-byte alignment");
_Static_assert(sizeof(kmalloc_block_t) % KMALLOC_ALIGN == 0,
               "kmalloc_block_t size must be a multiple of KMALLOC_ALIGN");


static kmalloc_block_t *g_free_list   = NULL;
static bool             g_initialized = false;


static inline size_t align_up_sz(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* Insert a block at the head of the free list. */
static void free_list_insert(kmalloc_block_t *b) {
    b->free      = 1;
    b->magic     = KMALLOC_MAGIC_FREE;
    b->next_free = g_free_list;
    b->prev_free = NULL;
    if (g_free_list) {
        g_free_list->prev_free = b;
    }
    g_free_list = b;
}

/* Remove a block from the free list (it is about to be handed out). */
static void free_list_remove(kmalloc_block_t *b) {
    if (b->prev_free) {
        b->prev_free->next_free = b->next_free;
    } else {
        g_free_list = b->next_free;
    }
    if (b->next_free) {
        b->next_free->prev_free = b->prev_free;
    }
    b->next_free = NULL;
    b->prev_free = NULL;
}


static bool expand_pool(void) {
    void *pool = vmalloc(KMALLOC_POOL_SIZE, VMALLOC_DEFAULT_FLAGS);
    if (!pool) {
        printk("[kmalloc] expand_pool: vmalloc failed\n");
        return false;
    }

    kmalloc_block_t *block = (kmalloc_block_t *)pool;
    block->size      = KMALLOC_POOL_SIZE - sizeof(kmalloc_block_t);
    block->next_phys = NULL;
    free_list_insert(block);

    printk("[kmalloc] new pool: %llu MiB at 0x%llx\n",
           (unsigned long long)(KMALLOC_POOL_SIZE / (1024ULL * 1024ULL)),
           (unsigned long long)(uintptr_t)pool);
    return true;
}



void kmalloc_init(void) {
    if (g_initialized) {
        return;
    }
    if (!expand_pool()) {
        printk("[kmalloc] init failed\n");
        return;
    }
    g_initialized = true;
    printk("[kmalloc] ready\n");
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (!g_initialized) {
        kmalloc_init();
        if (!g_initialized) {
            return NULL;
        }
    }

    
    size = align_up_sz(size, KMALLOC_ALIGN);

    /* Two attempts: first with the current pool, then after expansion.   */
    for (int attempt = 0; attempt < 2; attempt++) {

        for (kmalloc_block_t *b = g_free_list; b != NULL; b = b->next_free) {

            if (b->size < size) {
                continue;
            }

            /* --- Split the block if the leftover is large enough --- */
            size_t leftover = b->size - size;
            if (leftover >= KMALLOC_MIN_SPLIT) {
                /* Carve a new header out of the tail of this block. */
                kmalloc_block_t *split =
                    (kmalloc_block_t *)((uint8_t *)(b + 1) + size);

                split->size      = leftover - sizeof(kmalloc_block_t);
                split->next_phys = b->next_phys;

                b->next_phys = split;
                b->size      = size;

                free_list_insert(split);
            }

            /* --- Hand the block out --- */
            free_list_remove(b);
            b->free  = 0;
            b->magic = KMALLOC_MAGIC_USED;
            return (void *)(b + 1);
        }

        /* First pass failed: try to expand the heap and retry once. */
        if (attempt == 0 && !expand_pool()) {
            break;
        }
    }

    printk("[kmalloc] OOM: could not allocate %zu bytes\n", size);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    /* The header lives in the 48 bytes immediately before the user pointer. */
    kmalloc_block_t *block = (kmalloc_block_t *)ptr - 1;

    /* --- Integrity checks --- */
    if (block->magic == KMALLOC_MAGIC_FREE) {
        printk("[kmalloc] kfree: double-free detected at 0x%llx\n",
               (unsigned long long)(uintptr_t)ptr);
        return;
    }
    if (block->magic != KMALLOC_MAGIC_USED) {
        printk("[kmalloc] kfree: bad magic 0x%08x at 0x%llx "
               "(corruption or invalid pointer?)\n",
               block->magic,
               (unsigned long long)(uintptr_t)ptr);
        return;
    }
    if (block->free) {
        printk("[kmalloc] kfree: block marked free but magic is USED at 0x%llx\n",
               (unsigned long long)(uintptr_t)ptr);
        return;
    }

    /* Return the block to the free list. */
    free_list_insert(block);

    /* --- Forward coalescing ---
     *
     * If the physically-next block is also free, merge them into one.
     * We can repeat this to greedily absorb a run of free blocks.       */
    while (block->next_phys != NULL && block->next_phys->free) {
        kmalloc_block_t *next = block->next_phys;

        /* Absorb next: reclaim its header bytes into this block's payload. */
        free_list_remove(next);
        block->size      += sizeof(kmalloc_block_t) + next->size;
        block->next_phys  = next->next_phys;

        /* Invalidate the absorbed header so stale pointers are caught. */
        next->magic = 0;
    }
}