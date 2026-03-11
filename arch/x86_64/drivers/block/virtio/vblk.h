#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * VirtIO Block Driver — header
 *
 * Supports legacy (transitional) VirtIO 1.0 devices as exposed by QEMU
 * via -device virtio-blk-pci.
 *
 * PCI identity:
 *   Vendor  : 0x1AF4  (Red Hat / VirtIO)
 *   Device  : 0x1001  (legacy block)  /  0x1042  (modern block)
 *   Class   : 0x01  Subclass: 0x00  (SCSI / block storage controller)
 */

/* -----------------------------------------------------------------------
 * VirtIO PCI config-space layout (legacy / transitional BAR0, I/O port)
 * Offsets relative to the virtio-specific region (after the common 20-byte
 * PCI config header area that the device exposes on BAR0 as I/O ports).
 * ----------------------------------------------------------------------- */
#define VIRTIO_PCI_HOST_FEATURES   0x00   /* R    - 32-bit feature bits offered by device */
#define VIRTIO_PCI_GUEST_FEATURES  0x04   /* RW   - 32-bit feature bits accepted by driver */
#define VIRTIO_PCI_QUEUE_PFN       0x08   /* RW   - guest-physical page frame of virtqueue */
#define VIRTIO_PCI_QUEUE_SIZE      0x0C   /* R    - number of entries in the selected queue */
#define VIRTIO_PCI_QUEUE_SEL       0x0E   /* RW   - queue index to configure */
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10   /* W    - write queue index to kick device */
#define VIRTIO_PCI_STATUS          0x12   /* RW   - device status register */
#define VIRTIO_PCI_ISR             0x13   /* R    - ISR status (clears on read) */

/* Device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  (1 << 0)
#define VIRTIO_STATUS_DRIVER       (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK    (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK  (1 << 3)
#define VIRTIO_STATUS_FAILED       (1 << 7)

/* Feature flags for virtio-blk */
#define VIRTIO_BLK_F_SIZE_MAX      (1 << 1)   /* max segment size present */
#define VIRTIO_BLK_F_SEG_MAX       (1 << 2)   /* max segment count present */
#define VIRTIO_BLK_F_GEOMETRY      (1 << 4)   /* legacy geometry present */
#define VIRTIO_BLK_F_RO            (1 << 5)   /* device is read-only */
#define VIRTIO_BLK_F_BLK_SIZE      (1 << 6)   /* block size field present */
#define VIRTIO_BLK_F_FLUSH         (1 << 9)   /* supports FLUSH command */

/* -----------------------------------------------------------------------
 * Virtqueue descriptor / available / used ring structures
 * (split virtqueue, the only layout we need for now)
 * ----------------------------------------------------------------------- */
#define VIRTQ_DESC_F_NEXT     (1 << 0)   /* descriptor chains to next */
#define VIRTQ_DESC_F_WRITE    (1 << 1)   /* device writes into this buffer */

typedef struct {
    uint64_t addr;   /* physical address of the buffer */
    uint32_t len;
    uint16_t flags;
    uint16_t next;   /* index of next descriptor (if F_NEXT) */
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;    /* next slot the driver will write */
    uint16_t ring[]; /* descriptor indices — VLA, sized to queue_size */
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;     /* descriptor chain head index */
    uint32_t len;    /* total bytes written by device */
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];  /* VLA, sized to queue_size */
} __attribute__((packed)) virtq_used_t;


#define VIRTIO_BLK_T_IN      0   /* read  */
#define VIRTIO_BLK_T_OUT     1   /* write */
#define VIRTIO_BLK_T_FLUSH   4

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;   /* 512-byte sector index */
} __attribute__((packed)) virtio_blk_req_hdr_t;

/* Status byte written by device at end of request */
#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

/* -----------------------------------------------------------------------
 * Driver state
 * ----------------------------------------------------------------------- */
#define VIRTIO_BLK_QUEUE_SIZE  128   /* must be power-of-two, ≤ device's queue_size */

typedef struct {
    uint16_t    io_base;         /* I/O port base from BAR0 */
    uint64_t    capacity;        /* device capacity in 512-byte sectors */
    uint32_t    blk_size;        /* logical block size (usually 512) */
    bool        read_only;

    /* Virtqueue 0 (the only requestq for virtio-blk) */
    virtq_desc_t  *desc;         /* descriptor table   (queue_size entries) */
    virtq_avail_t *avail;        /* driver → device    */
    virtq_used_t  *used;         /* device → driver    */

    uint16_t    queue_size;      /* negotiated queue size */
    uint16_t    free_head;       /* head of free descriptor list */
    uint16_t    last_used_idx;   /* shadow of used->idx for polling */

    uint64_t  dma_phys;      // physical base of DMA region 
    uintptr_t dma_virt; 
} virtio_blk_dev_t;

/* Public interface */
bool virtio_blk_init(void);
bool virtio_blk_read (uint64_t sector, uint32_t count, void *buf);
bool virtio_blk_write(uint64_t sector, uint32_t count, const void *buf);