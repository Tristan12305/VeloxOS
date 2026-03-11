/*
 * arch/x86_64/drivers/block/virtio_blk.c
 *
 * VirtIO block device driver — legacy (transitional) PCI interface.
 *
 * QEMU invocation:
 *   -device virtio-blk-pci,drive=hd0 -drive id=hd0,file=disk.img,format=raw
 *
 * Initialisation sequence (virtio 1.0 §3.1, legacy subset):
 *   1. Find the device via PCI scan (vendor 0x1AF4, device 0x1001 or 0x1042).
 *   2. Reset the device (write 0 to status register).
 *   3. Set ACKNOWLEDGE + DRIVER status bits.
 *   4. Read feature bits; negotiate a subset; set FEATURES_OK.
 *   5. Read device config (capacity, block size …).
 *   6. Set up virtqueue 0.
 *   7. Set DRIVER_OK.
 */

#include "vblk.h"
#include <arch/x86_64/drivers/block/pci.h>
#include <lib/string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <include/printk.h>
#include <mm/vmalloc.h>
#include <boot/boot.h>   /* g_hhdm_offset */

//I/O port helpers (BAR0 of a legacy VirtIO PCI device is I/O space)

#include <mm/pmm.h>




static inline void _outb(uint16_t port, uint8_t  v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline void _outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
static inline void _outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t  _inb(uint16_t port) { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint16_t _inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint32_t _inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

/* Read/write the VirtIO device registers (relative to io_base) */
#define vio_read8(dev,  off) _inb ((dev)->io_base + (off))
#define vio_read16(dev, off) _inw ((dev)->io_base + (off))
#define vio_read32(dev, off) _inl ((dev)->io_base + (off))
#define vio_write8(dev,  off, v) _outb((dev)->io_base + (off), (v))
#define vio_write16(dev, off, v) _outw((dev)->io_base + (off), (v))
#define vio_write32(dev, off, v) _outl((dev)->io_base + (off), (v))

 // VirtIO PCI IDs

#define VIRTIO_VENDOR_ID        0x1AF4u
#define VIRTIO_DEV_BLOCK_LEGACY 0x1001u   /* transitional / legacy */
#define VIRTIO_DEV_BLOCK_MODERN 0x1042u   /* virtio 1.0+ only      */





#define VIRTIO_PCI_CONFIG_OFF   20   /* no MSI-X; with MSI-X it is 24 */

#define BLKCFG_CAPACITY_LO  (VIRTIO_PCI_CONFIG_OFF + 0x00)   /* u32 lo of capacity (sectors) */
#define BLKCFG_CAPACITY_HI  (VIRTIO_PCI_CONFIG_OFF + 0x04)   /* u32 hi of capacity (sectors) */
#define BLKCFG_SIZE_MAX     (VIRTIO_PCI_CONFIG_OFF + 0x08)
#define BLKCFG_SEG_MAX      (VIRTIO_PCI_CONFIG_OFF + 0x0C)
#define BLKCFG_BLK_SIZE     (VIRTIO_PCI_CONFIG_OFF + 0x14)   /* logical block size */


#define DRIVER_FEATURES  (VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH)

#define VIRTIO_PAGE_SIZE  4096u
#define VIRTIO_PAGE_SHIFT 12u


static virtio_blk_dev_t g_virtio_blk;
static bool             g_virtio_blk_ready = false;


static void pci_enable_device(pci_device_t *dev) {
    /*
     * We need to write back to PCI config space.
     * pci.h only exposes read helpers, so we replicate the write here.
     * Command register is at config offset 0x04.
     */
    /* Build the config-space address for this device's command register */
    uint32_t addr = (1U << 31) |
                    ((uint32_t)dev->bus      << 16) |
                    ((uint32_t)dev->device   << 11) |
                    ((uint32_t)dev->function <<  8) |
                    (0x04U & 0xFCU);

    /* Read–modify–write: set bits 0 (I/O space), 1 (memory), 2 (bus master) */
    __asm__ volatile("outl %0, %1" :: "a"(addr),         "Nd"((uint16_t)0xCF8));
    uint32_t cmd;
    __asm__ volatile("inl %1, %0" :  "=a"(cmd) :         "Nd"((uint16_t)0xCFC));
    cmd |= (1U << 0) | (1U << 1) | (1U << 2);
    __asm__ volatile("outl %0, %1" :: "a"(addr),         "Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(cmd),          "Nd"((uint16_t)0xCFC));
}




/* Size helpers from the spec */
static inline size_t vq_desc_size (uint16_t qs) { return 16u * qs; }
static inline size_t vq_avail_size(uint16_t qs) { return 6u + 2u * qs; }
static inline size_t vq_used_size (uint16_t qs) { return 6u + 8u * qs; }


static bool virtq_setup(virtio_blk_dev_t *dev) {
    vio_write16(dev, VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t qs = vio_read16(dev, VIRTIO_PCI_QUEUE_SIZE);
    if (qs == 0) { printk("[virtio_blk] queue size 0\n"); return false; }
    if (qs > VIRTIO_BLK_QUEUE_SIZE) qs = VIRTIO_BLK_QUEUE_SIZE;
    dev->queue_size = qs;
    printk("[virtio_blk] queue size = %u\n", (unsigned)qs);

    size_t desc_avail_bytes = vq_desc_size(qs) + vq_avail_size(qs);
    size_t used_offset = (desc_avail_bytes + VIRTIO_PAGE_SIZE - 1)
                         & ~(size_t)(VIRTIO_PAGE_SIZE - 1);
    size_t total_bytes = used_offset + vq_used_size(qs);

    /* Allocate physically contiguous pages directly from PMM.
     * Access via HHDM so phys = virt - g_hhdm_offset holds exactly. */
    size_t total_pages = (total_bytes + VIRTIO_PAGE_SIZE - 1) / VIRTIO_PAGE_SIZE;

    /* Allocate pages one at a time and verify they are contiguous.
     * PMM hands pages out sequentially so this nearly always succeeds. */
    uint64_t first_phys = PMM_INVALID_PHYS_ADDR;
    for (size_t i = 0; i < total_pages; i++) {
        uint64_t phys = PMM_INVALID_PHYS_ADDR;
        if (!pmm_try_alloc_page_phys(&phys)) {
            printk("[virtio_blk] PMM out of memory for virtqueue\n");
            return false;
        }
        if (i == 0) {
            first_phys = phys;
        } else if (phys != first_phys + i * VIRTIO_PAGE_SIZE) {
            printk("[virtio_blk] PMM pages not contiguous, giving up\n");
            return false;
        }
    }

    /* Virtual address through HHDM — guaranteed to be the right physical page */
    uintptr_t virt_base = (uintptr_t)(g_hhdm_offset + first_phys);

    __builtin_memset((void *)virt_base, 0, total_bytes);

    dev->desc  = (virtq_desc_t  *) virt_base;
    dev->avail = (virtq_avail_t *)(virt_base + vq_desc_size(qs));
    dev->used  = (virtq_used_t  *)(virt_base + used_offset);

    for (uint16_t i = 0; i < qs - 1; i++) {
        dev->desc[i].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[i].next  = (uint16_t)(i + 1);
    }
    dev->desc[qs - 1].flags = 0;
    dev->desc[qs - 1].next  = 0;
    dev->free_head     = 0;
    dev->last_used_idx = 0;

    uint32_t pfn = (uint32_t)(first_phys >> VIRTIO_PAGE_SHIFT);
    vio_write32(dev, VIRTIO_PCI_QUEUE_PFN, pfn);

    printk("[virtio_blk] virtqueue: virt=0x%llx phys=0x%llx pfn=%u\n",
           (unsigned long long)virt_base,
           (unsigned long long)first_phys,
           (unsigned)pfn);
    return true;
}

bool virtio_blk_init(void) {
    /* Step 1 — locate the device on the PCI bus */
    pci_device_t *pci = pci_find_by_id(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLOCK_LEGACY);
    if (!pci) {
        pci = pci_find_by_id(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLOCK_MODERN);
    }
    if (!pci) {
        printk("[virtio_blk] no VirtIO block device found on PCI bus\n");
        return false;
    }
    printk("[virtio_blk] found device at %02x:%02x.%x  "
           "vendor=%04x device=%04x\n",
           pci->bus, pci->device, pci->function,
           pci->vendor_id, pci->device_id);

    /*
     * BAR0 for a legacy VirtIO device is an I/O-space BAR (bit 0 = 1).
     * Strip the indicator bits to get the base port.
     */
    if (!(pci->bars[0] & 0x1)) {
        printk("[virtio_blk] BAR0 is not I/O space — modern MMIO device "
               "not yet supported\n");
        return false;
    }
    uint16_t io_base = (uint16_t)(pci->bars[0] & ~0x3u);
    printk("[virtio_blk] I/O base = 0x%04x\n", (unsigned)io_base);

    /* Enable I/O space + bus mastering in PCI command register */
    pci_enable_device(pci);

    virtio_blk_dev_t *dev = &g_virtio_blk;
    dev->io_base = io_base;

    /* Step 2 — reset the device */
    vio_write8(dev, VIRTIO_PCI_STATUS, 0);

    /* Step 3 — ACKNOWLEDGE + DRIVER */
    vio_write8(dev, VIRTIO_PCI_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Step 4 — feature negotiation */
    uint32_t host_features = vio_read32(dev, VIRTIO_PCI_HOST_FEATURES);
    printk("[virtio_blk] host features = 0x%08x\n", (unsigned)host_features);

    uint32_t guest_features = host_features & DRIVER_FEATURES;
    vio_write32(dev, VIRTIO_PCI_GUEST_FEATURES, guest_features);
    printk("[virtio_blk] negotiated features = 0x%08x\n", (unsigned)guest_features);

    /* Step 5 — read device config */
    uint32_t cap_lo = vio_read32(dev, BLKCFG_CAPACITY_LO);
    uint32_t cap_hi = vio_read32(dev, BLKCFG_CAPACITY_HI);
    dev->capacity   = ((uint64_t)cap_hi << 32) | cap_lo;

    dev->blk_size = (guest_features & VIRTIO_BLK_F_BLK_SIZE)
                    ? vio_read32(dev, BLKCFG_BLK_SIZE)
                    : 512u;

    dev->read_only = !!(host_features & VIRTIO_BLK_F_RO);

    printk("[virtio_blk] capacity = %llu sectors (%llu MiB), "
           "block_size = %u, read_only = %d\n",
           (unsigned long long)dev->capacity,
           (unsigned long long)(dev->capacity / 2048),
           (unsigned)dev->blk_size,
           (int)dev->read_only);

    /* Step 6 — set up virtqueue 0 */
    if (!virtq_setup(dev)) {
        vio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    // allocate enough pages for header + max data + status
// 1 header page is plenty for now (supports up to ~4080 bytes of data per request)
    uint64_t dma_phys = PMM_INVALID_PHYS_ADDR;
    if (!pmm_try_alloc_page_phys(&dma_phys)) {
        printk("[virtio_blk] failed to alloc DMA page\n");
        return false;
    }
    dev->dma_phys = dma_phys;
    dev->dma_virt = (uintptr_t)(g_hhdm_offset + dma_phys);
    memset((void *)dev->dma_virt, 0, 4096);

    vio_write8(dev, VIRTIO_PCI_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE |
               VIRTIO_STATUS_DRIVER      |
               VIRTIO_STATUS_DRIVER_OK);

    g_virtio_blk_ready = true;
    printk("[virtio_blk] initialised successfully\n");
    return true;
}




bool virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    if (!g_virtio_blk_ready) return false;

    virtio_blk_dev_t *dev = &g_virtio_blk;
    uint32_t len = count * dev->blk_size;

    // DMA region layout within the one allocated page:
    //   offset 0    : virtio_blk_req_hdr_t (16 bytes)
    //   offset 16   : data buffer          (len bytes)
    //   offset 16+len: status byte         (1 byte)
    uint64_t hdr_phys    = dev->dma_phys;
    uint64_t buf_phys    = dev->dma_phys + 16;
    uint64_t status_phys = dev->dma_phys + 16 + len;

    virtio_blk_req_hdr_t *hdr = (virtio_blk_req_hdr_t *)dev->dma_virt;
    volatile uint8_t     *status = (volatile uint8_t *)(dev->dma_virt + 16 + len);

    hdr->type     = VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status = 0xFF;

    // grab 3 descriptors
    uint16_t d0 = dev->free_head;
    uint16_t d1 = dev->desc[d0].next;
    uint16_t d2 = dev->desc[d1].next;
    dev->free_head = dev->desc[d2].next;

    dev->desc[d0].addr  = hdr_phys;
    dev->desc[d0].len   = sizeof(virtio_blk_req_hdr_t);
    dev->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    dev->desc[d0].next  = d1;

    dev->desc[d1].addr  = buf_phys;
    dev->desc[d1].len   = len;
    dev->desc[d1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    dev->desc[d1].next  = d2;

    dev->desc[d2].addr  = status_phys;
    dev->desc[d2].len   = 1;
    dev->desc[d2].flags = VIRTQ_DESC_F_WRITE;
    dev->desc[d2].next  = 0;

    uint16_t avail_idx = dev->avail->idx & (dev->queue_size - 1);
    dev->avail->ring[avail_idx] = d0;
    __asm__ volatile("" ::: "memory");
    dev->avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    vio_write16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    while (dev->used->idx == dev->last_used_idx)
        __asm__ volatile("pause" ::: "memory");
    dev->last_used_idx++;

    dev->desc[d2].next = dev->free_head;
    dev->free_head = d0;

    if (*status != VIRTIO_BLK_S_OK) {
        printk("[virtio_blk] read error: status=%u\n", (unsigned)*status);
        return false;
    }

    // copy from DMA bounce buffer into caller's buffer
    memcpy(buf, (void *)(dev->dma_virt + 16), len);
    return true;
}



bool virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    if (!g_virtio_blk_ready) return false;
    if (g_virtio_blk.read_only) {
        printk("[virtio_blk] device is read-only\n");
        return false;
    }

    virtio_blk_dev_t *dev = &g_virtio_blk;
    uint32_t len = count * dev->blk_size;

    uint64_t hdr_phys    = dev->dma_phys;
    uint64_t buf_phys    = dev->dma_phys + 16;
    uint64_t status_phys = dev->dma_phys + 16 + len;

    virtio_blk_req_hdr_t *hdr    = (virtio_blk_req_hdr_t *)dev->dma_virt;
    volatile uint8_t     *status = (volatile uint8_t *)(dev->dma_virt + 16 + len);

    hdr->type     = VIRTIO_BLK_T_OUT;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status = 0xFF;

    // copy caller's data into DMA buffer BEFORE submitting
    memcpy((void *)(dev->dma_virt + 16), buf, len);

    uint16_t d0 = dev->free_head;
    uint16_t d1 = dev->desc[d0].next;
    uint16_t d2 = dev->desc[d1].next;
    dev->free_head = dev->desc[d2].next;

    dev->desc[d0].addr  = hdr_phys;
    dev->desc[d0].len   = sizeof(virtio_blk_req_hdr_t);
    dev->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    dev->desc[d0].next  = d1;

    dev->desc[d1].addr  = buf_phys;
    dev->desc[d1].len   = len;
    dev->desc[d1].flags = VIRTQ_DESC_F_NEXT;  // no WRITE — device reads
    dev->desc[d1].next  = d2;

    dev->desc[d2].addr  = status_phys;
    dev->desc[d2].len   = 1;
    dev->desc[d2].flags = VIRTQ_DESC_F_WRITE;
    dev->desc[d2].next  = 0;

    uint16_t avail_idx = dev->avail->idx & (dev->queue_size - 1);
    dev->avail->ring[avail_idx] = d0;
    __asm__ volatile("" ::: "memory");
    dev->avail->idx++;
    __asm__ volatile("" ::: "memory");

    vio_write16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    while (dev->used->idx == dev->last_used_idx)
        __asm__ volatile("pause");
    dev->last_used_idx++;

    dev->desc[d2].next = dev->free_head;
    dev->free_head = d0;

    if (*status != VIRTIO_BLK_S_OK) {
        printk("[virtio_blk] write error: status=%u\n", (unsigned)*status);
        return false;
    }
    return true;
}