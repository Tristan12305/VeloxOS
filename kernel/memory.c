#include <kernel/init.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/vmalloc.h>
#include <include/kmalloc.h>
#include <boot/boot.h>

void mem_init(void) {
    pmm_init(&g_memory_map);
    paging_init(&g_memory_map);
    vmalloc_init();
    //kmalloc_init();
}