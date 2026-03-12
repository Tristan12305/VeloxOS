/*
 * arch/x86_64/drivers/block/virtio/vblk.c
 *
 * VirtIO block device driver — legacy (transitional) PCI interface.
 *
 * QEMU invocation:
 *   -device virtio-blk-pci,drive=hd0 -drive id=hd0,file=disk.img,format=raw
 */

#include "vblk.h"
#include <arch/x86_64/drivers/block/pci.h>
#include <lib/string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <include/printk.h>
#include <mm/vmalloc.h>
#include <mm/pmm.h>
#include <boot/boot.h>



static inline void     _outb(uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     _outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void     _outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  _inb (uint16_t p){ uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t _inw (uint16_t p){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t _inl (uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

#define vio_read8(dev,  off)    _inb ((dev)->io_base + (off))
#define vio_read16(dev, off)    _inw ((dev)->io_base + (off))
#define vio_read32(dev, off)    _inl ((dev)->io_base + (off))
#define vio_write8(dev,  off,v) _outb((dev)->io_base + (off), (v))
#define vio_write16(dev, off,v) _outw((dev)->io_base + (off), (v))
#define vio_write32(dev, off,v) _outl((dev)->io_base + (off), (v))



#define VIRTIO_VENDOR_ID        0x1AF4u
#define VIRTIO_DEV_BLOCK_LEGACY 0x1001u
#define VIRTIO_DEV_BLOCK_MODERN 0x1042u


#define VIRTIO_PCI_CONFIG_OFF   20

#define BLKCFG_CAPACITY_LO  (VIRTIO_PCI_CONFIG_OFF + 0x00)
#define BLKCFG_CAPACITY_HI  (VIRTIO_PCI_CONFIG_OFF + 0x04)
#define BLKCFG_BLK_SIZE     (VIRTIO_PCI_CONFIG_OFF + 0x14)

#define DRIVER_FEATURES  (VIRTIO_BLK_F_BLK_SIZE)

#define VIRTIO_PAGE_SIZE  4096u
#define VIRTIO_PAGE_SHIFT 12u

/* Timeout iterations for the poll loop */
#define VIRTIO_POLL_TIMEOUT 50000000u



static virtio_blk_dev_t g_virtio_blk;
static bool             g_virtio_blk_ready = false;

extern uint64_t g_hhdm_offset;


static void pci_enable_device(pci_device_t *dev) {
    uint32_t addr = (1U << 31) |
                    ((uint32_t)dev->bus      << 16) |
                    ((uint32_t)dev->device   << 11) |
                    ((uint32_t)dev->function <<  8) |
                    (0x04U & 0xFCU);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t cmd;
    __asm__ volatile("inl %1, %0"  : "=a"(cmd)  : "Nd"((uint16_t)0xCFC));
    cmd |= (1U << 0) | (1U << 1) | (1U << 2);
    __asm__ volatile("outl %0, %1" :: "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ volatile("outl %0, %1" :: "a"(cmd),  "Nd"((uint16_t)0xCFC));
}


static inline size_t vq_desc_size (uint16_t qs) { return 16u * qs; }
static inline size_t vq_avail_size(uint16_t qs) { return 6u + 2u * qs; }
static inline size_t vq_used_size (uint16_t qs) { return 6u + 8u * qs; }



static bool virtq_setup(virtio_blk_dev_t *dev) {
    vio_write16(dev, VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t qs = vio_read16(dev, VIRTIO_PCI_QUEUE_SIZE);
    if (qs == 0) {
        printk("[virtio_blk] device reports queue size 0\n");
        return false;
    }
    if (qs > VIRTIO_BLK_QUEUE_SIZE) qs = VIRTIO_BLK_QUEUE_SIZE;
    dev->queue_size = qs;
    printk("[virtio_blk] queue size = %u\n", (unsigned)qs);

    size_t desc_avail_bytes = vq_desc_size(qs) + vq_avail_size(qs);
    size_t used_offset      = (desc_avail_bytes + VIRTIO_PAGE_SIZE - 1)
                              & ~(size_t)(VIRTIO_PAGE_SIZE - 1);
    size_t total_bytes      = used_offset + vq_used_size(qs);
    size_t total_pages      = (total_bytes + VIRTIO_PAGE_SIZE - 1) / VIRTIO_PAGE_SIZE;

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
            printk("[virtio_blk] PMM pages not contiguous\n");
            return false;
        }
    }

    uintptr_t virt_base = (uintptr_t)(g_hhdm_offset + first_phys);
    __builtin_memset((void *)virt_base, 0, total_bytes);

    dev->desc  = (virtq_desc_t  *) virt_base;
    dev->avail = (virtq_avail_t *)(virt_base + vq_desc_size(qs));
    dev->used  = (volatile virtq_used_t  *)(virt_base + used_offset);

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

    // after vio_write32(dev, VIRTIO_PCI_QUEUE_PFN, pfn) in virtq_setup:
    uint32_t pfn_readback = vio_read32(dev, VIRTIO_PCI_QUEUE_PFN);
    printk("[virtio_blk] PFN readback = %u (expected %u)\n",
           (unsigned)pfn_readback, (unsigned)pfn);

    printk("[virtio_blk] virtqueue: virt=0x%llx phys=0x%llx pfn=%u\n",
           (unsigned long long)virt_base,
           (unsigned long long)first_phys,
           (unsigned)pfn);
    return true;
}



bool virtio_blk_init(void) {
    pci_device_t *pci = pci_find_by_id(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLOCK_LEGACY);
    if (!pci)
        pci = pci_find_by_id(VIRTIO_VENDOR_ID, VIRTIO_DEV_BLOCK_MODERN);
    if (!pci) {
        printk("[virtio_blk] no VirtIO block device found\n");
        return false;
    }

    printk("[virtio_blk] found device at %x:%x.%x vendor=%x device=%x\n",
           (unsigned)pci->bus, (unsigned)pci->device, (unsigned)pci->function,
           (unsigned)pci->vendor_id, (unsigned)pci->device_id);

    printk("[virtio_blk] BAR0 raw = 0x%x\n", (unsigned)pci->bars[0]);

    if (!(pci->bars[0] & 0x1)) {
        printk("[virtio_blk] BAR0 is not I/O space\n");
        return false;
    }

    uint16_t io_base = (uint16_t)(pci->bars[0] & ~0x3u);
    printk("[virtio_blk] I/O base = 0x%x\n", (unsigned)io_base);

    pci_enable_device(pci);

    virtio_blk_dev_t *dev = &g_virtio_blk;
    dev->io_base = io_base;

    /* Reset */
    vio_write8(dev, VIRTIO_PCI_STATUS, 0);

    /* ACKNOWLEDGE + DRIVER */
    vio_write8(dev, VIRTIO_PCI_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    uint32_t host_features = vio_read32(dev, VIRTIO_PCI_HOST_FEATURES);
    printk("[virtio_blk] host features = 0x%x\n", (unsigned)host_features);

    uint32_t guest_features = host_features & DRIVER_FEATURES;
    vio_write32(dev, VIRTIO_PCI_GUEST_FEATURES, guest_features);
    printk("[virtio_blk] negotiated features = 0x%x\n", (unsigned)guest_features);

    /* Device config */
    uint32_t cap_lo = vio_read32(dev, BLKCFG_CAPACITY_LO);
    uint32_t cap_hi = vio_read32(dev, BLKCFG_CAPACITY_HI);
    dev->capacity   = ((uint64_t)cap_hi << 32) | cap_lo;

    dev->blk_size  = (guest_features & VIRTIO_BLK_F_BLK_SIZE)
                     ? vio_read32(dev, BLKCFG_BLK_SIZE) : 512u;
    dev->read_only = !!(host_features & VIRTIO_BLK_F_RO);

    printk("[virtio_blk] capacity = %llu sectors (%llu MiB), "
           "block_size = %u, read_only = %d\n",
           (unsigned long long)dev->capacity,
           (unsigned long long)(dev->capacity / 2048),
           (unsigned)dev->blk_size,
           (int)dev->read_only);

    /* Virtqueue */
    if (!virtq_setup(dev)) {
        vio_write8(dev, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* DMA bounce buffer — one PMM page accessed via HHDM */
    uint64_t dma_phys = PMM_INVALID_PHYS_ADDR;
    if (!pmm_try_alloc_page_phys(&dma_phys)) {
        printk("[virtio_blk] failed to alloc DMA page\n");
        return false;
    }
    dev->dma_phys = dma_phys;
    dev->dma_virt = (uintptr_t)(g_hhdm_offset + dma_phys);
    memset((void *)dev->dma_virt, 0, 4096);

    printk("[virtio_blk] DMA page: phys=0x%llx virt=0x%llx\n",
           (unsigned long long)dev->dma_phys,
           (unsigned long long)dev->dma_virt);

    /* DRIVER_OK */
    vio_write8(dev, VIRTIO_PCI_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE |
               VIRTIO_STATUS_DRIVER      |
               VIRTIO_STATUS_DRIVER_OK);

    // after writing DRIVER_OK:
    uint8_t status_readback = vio_read8(dev, VIRTIO_PCI_STATUS);
    printk("[virtio_blk] STATUS readback = 0x%x\n", (unsigned)status_readback);

    g_virtio_blk_ready = true;
    printk("[virtio_blk] initialised successfully\n");
    return true;
}

/* -----------------------------------------------------------------------
 * Internal: submit a 3-descriptor chain and poll for completion
 *
 * DMA page layout:
 *   [0  .. 15      ] virtio_blk_req_hdr_t  (device reads)
 *   [16 .. 16+len-1] data buffer            (device reads or writes)
 *   [16 + len      ] status byte            (device writes)
 * ----------------------------------------------------------------------- */
static bool virtio_submit_and_poll(virtio_blk_dev_t *dev,
                                   uint32_t type,
                                   uint64_t sector,
                                   uint32_t len) {
    uint64_t hdr_phys    = dev->dma_phys;
    uint64_t buf_phys    = dev->dma_phys + 16;
    uint64_t status_phys = dev->dma_phys + 16 + len;

    virtio_blk_req_hdr_t *hdr    = (virtio_blk_req_hdr_t *)dev->dma_virt;
    volatile uint8_t     *status = (volatile uint8_t *)(dev->dma_virt + 16 + len);

    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status = 0xFF;

    if (dev->free_head + 3 > dev->queue_size) {
        printk("[virtio_blk] not enough free descriptors\n");
        return false;
    }

    uint16_t d0 = dev->free_head;
    uint16_t d1 = dev->desc[d0].next;
    uint16_t d2 = dev->desc[d1].next;
    dev->free_head = dev->desc[d2].next;

    /* Descriptor 0: request header — device reads */
    dev->desc[d0].addr  = hdr_phys;
    dev->desc[d0].len   = sizeof(virtio_blk_req_hdr_t);
    dev->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    dev->desc[d0].next  = d1;

    /* Descriptor 1: data buffer — device reads (OUT) or writes (IN) */
    dev->desc[d1].addr  = buf_phys;
    dev->desc[d1].len   = len;
    dev->desc[d1].flags = (type == VIRTIO_BLK_T_IN)
                          ? (VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT)
                          : VIRTQ_DESC_F_NEXT;
    dev->desc[d1].next  = d2;

    /* Descriptor 2: status byte — device always writes */
    dev->desc[d2].addr  = status_phys;
    dev->desc[d2].len   = 1;
    dev->desc[d2].flags = VIRTQ_DESC_F_WRITE;
    dev->desc[d2].next  = 0;

    /* Post to available ring */
    uint16_t avail_idx = dev->avail->idx & (dev->queue_size - 1);
    dev->avail->ring[avail_idx] = d0;
    __asm__ volatile("" ::: "memory");
    dev->avail->idx++;
    __asm__ volatile("mfence" ::: "memory");

    /* Kick the device */
    printk("[virtio_blk] notifying device (queue_notify port=0x%x)...\n",
           (unsigned)(dev->io_base + VIRTIO_PCI_QUEUE_NOTIFY));
    vio_write16(dev, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
    uint8_t isr = vio_read8(dev, VIRTIO_PCI_ISR);
    printk("[virtio_blk] ISR after notify = 0x%x\n", (unsigned)isr);

    /* Poll with timeout */
    uint32_t timeout = VIRTIO_POLL_TIMEOUT;
    while (dev->used->idx == dev->last_used_idx) {
        __asm__ volatile("pause" ::: "memory");
        if (--timeout == 0) {
            printk("[virtio_blk] TIMEOUT waiting for completion\n");
            printk("[virtio_blk] used->idx=%u last_used=%u\n",
                   (unsigned)dev->used->idx, (unsigned)dev->last_used_idx);
            printk("[virtio_blk] avail->idx=%u avail->ring[0]=%u\n",
                   (unsigned)dev->avail->idx, (unsigned)dev->avail->ring[0]);
            printk("[virtio_blk] status byte = 0x%x\n", (unsigned)*status);
            printk("[virtio_blk] d0: addr=0x%llx len=%u flags=0x%x\n",
                   (unsigned long long)dev->desc[d0].addr,
                   (unsigned)dev->desc[d0].len,
                   (unsigned)dev->desc[d0].flags);
            printk("[virtio_blk] d1: addr=0x%llx len=%u flags=0x%x\n",
                   (unsigned long long)dev->desc[d1].addr,
                   (unsigned)dev->desc[d1].len,
                   (unsigned)dev->desc[d1].flags);
            printk("[virtio_blk] d2: addr=0x%llx len=%u flags=0x%x\n",
                   (unsigned long long)dev->desc[d2].addr,
                   (unsigned)dev->desc[d2].len,
                   (unsigned)dev->desc[d2].flags);
            printk("[virtio_blk] dma_phys=0x%llx io_base=0x%x\n",
                   (unsigned long long)dev->dma_phys,
                   (unsigned)dev->io_base);
            /* return descriptors before bailing */
            dev->desc[d2].next = dev->free_head;
            dev->free_head = d0;
            return false;
        }
    }
    dev->last_used_idx++;

    /* Return descriptors to free list */
    dev->desc[d2].next = dev->free_head;
    dev->free_head = d0;

    if (*status != VIRTIO_BLK_S_OK) {
        printk("[virtio_blk] request error: status = %u\n", (unsigned)*status);
        return false;
    }
    return true;
}



bool virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    if (!g_virtio_blk_ready) return false;

    virtio_blk_dev_t *dev = &g_virtio_blk;
    uint32_t len = count * dev->blk_size;

    printk("[virtio_blk] read sector=%llu count=%u len=%u\n",
           (unsigned long long)sector, (unsigned)count, (unsigned)len);

    if (!virtio_submit_and_poll(dev, VIRTIO_BLK_T_IN, sector, len))
        return false;

    /* Copy from DMA bounce buffer into caller's buffer */
    memcpy(buf, (void *)(dev->dma_virt + 16), len);
    printk("[virtio_blk] read complete\n");
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

    printk("[virtio_blk] write sector=%llu count=%u len=%u\n",
           (unsigned long long)sector, (unsigned)count, (unsigned)len);

    /* Copy caller's data into DMA bounce buffer before submitting */
    memcpy((void *)(dev->dma_virt + 16), buf, len);

    if (!virtio_submit_and_poll(dev, VIRTIO_BLK_T_OUT, sector, len))
        return false;

    printk("[virtio_blk] write complete\n");
    return true;
}