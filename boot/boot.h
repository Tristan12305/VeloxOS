
#pragma once

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * boot_info.h
 *
 * Exposes the two primary data structures populated by limine_init():
 *
 *   BootFramebuffer  - framebuffer address + geometry returned by Limine
 *   BootMemoryMap    - physical memory map returned by Limine
 *
 * Include this header anywhere you need access to either.
 * Call limine_init() exactly once, early in kernel_main, before using
 * the global instances g_framebuffer or g_memory_map.
 * ----------------------------------------------------------------------- */


/* -----------------------------------------------------------------------
 * Framebuffer
 * ----------------------------------------------------------------------- */

typedef struct {
    void    *address;       /* Virtual (identity-mapped) base address       */
    uint64_t width;         /* Horizontal resolution in pixels               */
    uint64_t height;        /* Vertical resolution in pixels                 */
    uint64_t pitch;         /* Bytes per scanline (may be > width * bpp/8)  */
    uint16_t bpp;           /* Bits per pixel (typically 32)                 */

    /* Sub-pixel layout for the pixel format Limine reports.
     * Each field is the bit offset of that colour channel within a pixel.
     * e.g. for BGRX8888: red_shift=16, green_shift=8, blue_shift=0        */
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
} BootFramebuffer;


/* -----------------------------------------------------------------------
 * Memory map
 * ----------------------------------------------------------------------- */

/* Memory map entry types, matching the Limine protocol constants.
 * USABLE is the only type safe to hand to a physical-page allocator.        */
typedef enum {
    MEMMAP_USABLE                 = 0,
    MEMMAP_RESERVED               = 1,
    MEMMAP_ACPI_RECLAIMABLE       = 2,
    MEMMAP_ACPI_NVS               = 3,
    MEMMAP_BAD_MEMORY             = 4,
    MEMMAP_BOOTLOADER_RECLAIMABLE = 5,  /* Free once you own the page table  */
    MEMMAP_KERNEL_AND_MODULES     = 6,
    MEMMAP_FRAMEBUFFER            = 7,
} MemoryMapType;

typedef struct {
    uint64_t      base;   /* Physical base address of this region            */
    uint64_t      length; /* Length in bytes                                 */
    MemoryMapType type;
} MemoryMapEntry;

typedef struct {
    MemoryMapEntry *entries; /* Flat kernel-owned copy of Limine entries      */
    size_t          count;   /* Number of valid entries in the array          */
} BootMemoryMap;


/* -----------------------------------------------------------------------
 * Globals populated by limine_init()
 * ----------------------------------------------------------------------- */

/* Declared here; defined in limine_init.c */
extern BootFramebuffer g_framebuffer;
extern BootMemoryMap   g_memory_map;
extern uint64_t        g_hhdm_offset;


/* -----------------------------------------------------------------------
 * Initialisation
 * ----------------------------------------------------------------------- */

/* Must be called once, before touching g_framebuffer or g_memory_map.
 * Panics (halts) if either required Limine response is absent.             */
void limine_init(void);
