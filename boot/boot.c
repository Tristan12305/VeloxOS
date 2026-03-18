

#include <include/limine.h>
#include "boot.h"


__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);


__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

#ifndef LIMINE_MP_REQUEST
#define LIMINE_MP_REQUEST LIMINE_SMP_REQUEST
#endif

__attribute__((used, section(".requests")))
static volatile struct LIMINE_MP(request) mp_request = {
    .id       = LIMINE_MP_REQUEST,
    .revision = 0,
    .flags    = 0,
};


/* Required start/end markers so Limine can scan the requests section.     */
__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;


BootFramebuffer g_framebuffer = {0};
BootMemoryMap   g_memory_map  = {0};
uint64_t        g_hhdm_offset = 0;
uint64_t        g_rsdp_address = 0;
struct LIMINE_MP(response) *g_mp_response = 0;

#define BOOT_MEMMAP_MAX_ENTRIES 512
static MemoryMapEntry g_memmap_storage[BOOT_MEMMAP_MAX_ENTRIES];


static void _halt(void) {
    __asm__ volatile (
        "cli\n"
        "1: hlt\n"
        "jmp 1b\n"
    );
    __builtin_unreachable();
}


void limine_init(void) {
    if (!hhdm_request.response) {
        _halt();
    }
    g_hhdm_offset = hhdm_request.response->offset;

    if (rsdp_request.response) {
        g_rsdp_address = (uint64_t)(uintptr_t)rsdp_request.response->address;
    }
    if (mp_request.response) {
        g_mp_response = mp_request.response;
    }

    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1) {
        _halt();
    }

    struct limine_framebuffer *fb =
        framebuffer_request.response->framebuffers[0];

    g_framebuffer.address         = fb->address;
    g_framebuffer.width           = fb->width;
    g_framebuffer.height          = fb->height;
    g_framebuffer.pitch           = fb->pitch;
    g_framebuffer.bpp             = fb->bpp;
    g_framebuffer.red_mask_size   = fb->red_mask_size;
    g_framebuffer.red_mask_shift  = fb->red_mask_shift;
    g_framebuffer.green_mask_size = fb->green_mask_size;
    g_framebuffer.green_mask_shift= fb->green_mask_shift;
    g_framebuffer.blue_mask_size  = fb->blue_mask_size;
    g_framebuffer.blue_mask_shift = fb->blue_mask_shift;

    if (!memmap_request.response) {
        _halt();
    }

    if (memmap_request.response->entry_count > BOOT_MEMMAP_MAX_ENTRIES) {
        _halt();
    }

    for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];
        if (!entry) {
            _halt();
        }

        g_memmap_storage[i].base   = entry->base;
        g_memmap_storage[i].length = entry->length;
        g_memmap_storage[i].type   = (MemoryMapType)entry->type;
    }

    g_memory_map.entries = g_memmap_storage;
    g_memory_map.count   = memmap_request.response->entry_count;
}
