/*
 * arch/x86_64/drivers/block/virtio/vblk.c
 *
 * Block device driver — ATA PIO (legacy IDE).
 *
 * Exposes the same virtio_blk_* API so callers don't need to change.
 *
 * QEMU invocation:
 *   -drive file=disk.img,format=raw,if=ide
 *
 * Only the primary ATA bus / master drive is supported for now.
 * LBA28 addressing — max 128 GiB, plenty for early development.
 */

#include "vblk.h"
#include <include/printk.h>
#include <lib/string.h>
#include <stdint.h>
#include <stdbool.h>


#define ATA_DATA        0x1F0   /* 16-bit data register                     */
#define ATA_ERROR       0x1F1   /* error register (read)                    */
#define ATA_FEATURES    0x1F1   /* features register (write)                */
#define ATA_SECTOR_CNT  0x1F2   /* sector count                             */
#define ATA_LBA_LO      0x1F3   /* LBA bits  7:0                            */
#define ATA_LBA_MID     0x1F4   /* LBA bits 15:8                            */
#define ATA_LBA_HI      0x1F5   /* LBA bits 23:16                           */
#define ATA_DRIVE_HEAD  0x1F6   /* drive / LBA bits 27:24                   */
#define ATA_STATUS      0x1F7   /* status register (read)                   */
#define ATA_COMMAND     0x1F7   /* command register (write)                 */

/* ATA status bits */
#define ATA_SR_BSY  0x80   /* busy               */
#define ATA_SR_DRQ  0x08   /* data request ready */
#define ATA_SR_ERR  0x01   /* error              */

/* ATA commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_FLUSH       0xE7


static inline void     _outb(uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     _outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  _inb (uint16_t p){ uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t _inw (uint16_t p){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }


/* Wait until the drive clears BSY. Returns false on timeout or no drive. */
static bool ata_wait_ready(void) {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = _inb(ATA_STATUS);
        if (status == 0xFF) return false;   /* floating bus — no drive */
        if (!(status & ATA_SR_BSY)) return true;
        __asm__ volatile("pause");
    }
    printk("[ata] timeout waiting for BSY to clear\n");
    return false;
}

/* Wait until DRQ is set. Returns false on error or timeout. */
static bool ata_wait_drq(void) {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = _inb(ATA_STATUS);
        if (status & ATA_SR_ERR) {
            printk("[ata] error: status=0x%x error=0x%x\n",
                   (unsigned)status, (unsigned)_inb(ATA_ERROR));
            return false;
        }
        if (status & ATA_SR_DRQ) return true;
        __asm__ volatile("pause");
    }
    printk("[ata] timeout waiting for DRQ\n");
    return false;
}

/* Select master drive and program LBA28 registers, then issue command. */
static void ata_setup_lba28(uint32_t lba, uint8_t count, uint8_t cmd) {
    /* 0xE0 = LBA mode (bit 6) + master drive (bit 4); low nibble = LBA[27:24] */
    _outb(ATA_DRIVE_HEAD,  0xE0 | ((lba >> 24) & 0x0F));
    _outb(ATA_SECTOR_CNT,  count);
    _outb(ATA_LBA_LO,  (uint8_t)(lba));
    _outb(ATA_LBA_MID, (uint8_t)(lba >>  8));
    _outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    _outb(ATA_COMMAND, cmd);
}


static bool g_ata_ready = false;


bool virtio_blk_init(void) {
    printk("[ata] initialising primary master...\n");

    _outb(ATA_DRIVE_HEAD, 0xA0);   /* select master */

    if (!ata_wait_ready()) {
        printk("[ata] no drive on primary bus\n");
        return false;
    }

    /* IDENTIFY DEVICE */
    _outb(ATA_DRIVE_HEAD, 0xA0);
    _outb(ATA_SECTOR_CNT, 0);
    _outb(ATA_LBA_LO,     0);
    _outb(ATA_LBA_MID,    0);
    _outb(ATA_LBA_HI,     0);
    _outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (_inb(ATA_STATUS) == 0) {
        printk("[ata] drive does not exist\n");
        return false;
    }

    if (!ata_wait_drq()) {
        uint8_t mid = _inb(ATA_LBA_MID);
        uint8_t hi  = _inb(ATA_LBA_HI);
        if (mid != 0 || hi != 0)
            printk("[ata] ATAPI device (not a plain disk)\n");
        return false;
    }

    /* Drain the 256 IDENTIFY words */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = _inw(ATA_DATA);

    /* Words 60+61 hold total LBA28 sector count */
    uint32_t total_sectors = ((uint32_t)identify[61] << 16) | identify[60];
    printk("[ata] drive ready: %u sectors (%u MiB)\n",
           (unsigned)total_sectors,
           (unsigned)(total_sectors / 2048));

    g_ata_ready = true;
    return true;
}

bool virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    if (!g_ata_ready) {
        printk("[ata] read: driver not ready\n");
        return false;
    }
    if (count == 0 || count > 255) {
        printk("[ata] read: invalid count %u\n", (unsigned)count);
        return false;
    }
    if (sector > 0x0FFFFFFF) {
        printk("[ata] read: LBA28 limit exceeded\n");
        return false;
    }

    printk("[ata] read lba=%llu count=%u\n",
           (unsigned long long)sector, (unsigned)count);

    if (!ata_wait_ready()) return false;
    ata_setup_lba28((uint32_t)sector, (uint8_t)count, ATA_CMD_READ_PIO);

    uint16_t *ptr = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq()) return false;
        /* Each sector is 512 bytes = 256 16-bit words */
        for (int w = 0; w < 256; w++)
            ptr[w] = _inw(ATA_DATA);
        ptr += 256;
    }

    printk("[ata] read complete\n");
    return true;
}


bool virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    if (!g_ata_ready) {
        printk("[ata] write: driver not ready\n");
        return false;
    }
    if (count == 0 || count > 255) {
        printk("[ata] write: invalid count %u\n", (unsigned)count);
        return false;
    }
    if (sector > 0x0FFFFFFF) {
        printk("[ata] write: LBA28 limit exceeded\n");
        return false;
    }

    printk("[ata] write lba=%llu count=%u\n",
           (unsigned long long)sector, (unsigned)count);

    if (!ata_wait_ready()) return false;
    ata_setup_lba28((uint32_t)sector, (uint8_t)count, ATA_CMD_WRITE_PIO);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq()) return false;
        for (int w = 0; w < 256; w++)
            _outw(ATA_DATA, ptr[w]);
        ptr += 256;
    }

    /* Flush write cache to disk */
    _outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (!ata_wait_ready()) return false;

    printk("[ata] write complete\n");
    return true;
}