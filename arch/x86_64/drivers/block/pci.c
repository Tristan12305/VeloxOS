#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <mm/vmalloc.h>
#include "pci.h"
#include <include/printk.h>
#include <arch/x86_64/cpu/vendor.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

pci_node_t *pci_device_list  = NULL;
size_t      pci_device_count = 0;

static inline void outl_u32(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint32_t inl_u32(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void pci_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (1U << 31)            |
                       ((uint32_t)bus      << 16) |
                       ((uint32_t)device   << 11) |
                       ((uint32_t)function <<  8) |
                       (offset & 0xFCU);
    outl_u32(PCI_CONFIG_ADDRESS, address);
    outl_u32(PCI_CONFIG_DATA, value);
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1U << 31)            |
                       ((uint32_t)bus      << 16) |
                       ((uint32_t)device   << 11) |
                       ((uint32_t)function <<  8) |
                       (offset & 0xFCU);
    outl_u32(PCI_CONFIG_ADDRESS, address);
    return inl_u32(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, device, function, offset);
    uint8_t  shift = (offset & 2U) * 8U;
    return (uint16_t)((value >> shift) & 0xFFFFU);
}

static uint8_t pci_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, device, function, offset);
    uint8_t  shift = (offset & 3U) * 8U;
    return (uint8_t)((value >> shift) & 0xFFU);
}

uint32_t pci_cfg_read32(const pci_device_t *dev, uint8_t offset) {
    if (!dev) return 0;
    return pci_read32(dev->bus, dev->device, dev->function, offset);
}

uint16_t pci_cfg_read16(const pci_device_t *dev, uint8_t offset) {
    if (!dev) return 0;
    return pci_read16(dev->bus, dev->device, dev->function, offset);
}

uint8_t pci_cfg_read8(const pci_device_t *dev, uint8_t offset) {
    if (!dev) return 0;
    return pci_read8(dev->bus, dev->device, dev->function, offset);
}

void pci_cfg_write32(const pci_device_t *dev, uint8_t offset, uint32_t value) {
    if (!dev) return;
    pci_write32(dev->bus, dev->device, dev->function, offset, value);
}

void pci_cfg_write16(const pci_device_t *dev, uint8_t offset, uint16_t value) {
    if (!dev) return;
    uint32_t cur = pci_read32(dev->bus, dev->device, dev->function, offset);
    uint8_t  shift = (offset & 2U) * 8U;
    uint32_t mask = 0xFFFFU << shift;
    uint32_t next = (cur & ~mask) | ((uint32_t)value << shift);
    pci_write32(dev->bus, dev->device, dev->function, offset, next);
}

void pci_cfg_write8(const pci_device_t *dev, uint8_t offset, uint8_t value) {
    if (!dev) return;
    uint32_t cur = pci_read32(dev->bus, dev->device, dev->function, offset);
    uint8_t  shift = (offset & 3U) * 8U;
    uint32_t mask = 0xFFU << shift;
    uint32_t next = (cur & ~mask) | ((uint32_t)value << shift);
    pci_write32(dev->bus, dev->device, dev->function, offset, next);
}

bool pci_find_capability(const pci_device_t *dev, uint8_t cap_id, uint8_t *out_offset) {
    if (!dev) return false;
    uint16_t status = pci_cfg_read16(dev, 0x06);
    if ((status & (1U << 4)) == 0) {
        return false;
    }

    uint8_t cap_ptr = pci_cfg_read8(dev, 0x34);
    for (uint8_t i = 0; i < 48 && cap_ptr >= 0x40; i++) {
        uint8_t id = pci_cfg_read8(dev, cap_ptr + 0x0);
        if (id == cap_id) {
            if (out_offset) {
                *out_offset = cap_ptr;
            }
            return true;
        }
        cap_ptr = pci_cfg_read8(dev, cap_ptr + 0x1);
    }
    return false;
}

static void pci_scan_bus(uint8_t bus);

static void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_read16(bus, device, function, 0x00);
    if (vendor == 0xFFFFU) return;

    pci_node_t *node = vmalloc(sizeof(pci_node_t), VMALLOC_DEFAULT_FLAGS);
    if (!node) {
        printk("[pci] vmalloc failed, dropping %02x:%02x.%x\n",
               bus, device, function);
        return;
    }

    pci_device_t *d = &node->dev;
    d->bus         = bus;
    d->device      = device;
    d->function    = function;
    d->vendor_id   = vendor;
    d->device_id   = pci_read16(bus, device, function, 0x02);
    d->command     = pci_read16(bus, device, function, 0x04);
    d->status      = pci_read16(bus, device, function, 0x06);
    d->class_code  = pci_read8 (bus, device, function, 0x0B);
    d->subclass    = pci_read8 (bus, device, function, 0x0A);
    d->prog_if     = pci_read8 (bus, device, function, 0x09);
    d->revision    = pci_read8 (bus, device, function, 0x08);
    d->header_type = pci_read8 (bus, device, function, 0x0E);
    d->irq_pin     = pci_read8 (bus, device, function, 0x3D);
    d->irq_line    = pci_read8 (bus, device, function, 0x3C);

    uint8_t type = d->header_type & 0x7F;

    if (type == 0x00) {
        /* endpoint: 6 BARs */
        for (int i = 0; i < 6; i++)
            d->bars[i] = pci_read32(bus, device, function, 0x10 + i * 4);

    } else if (type == 0x01) {
        /* PCI-to-PCI bridge: 2 BARs, then recurse into secondary bus */
        d->bars[0] = pci_read32(bus, device, function, 0x10);
        d->bars[1] = pci_read32(bus, device, function, 0x14);
        for (int i = 2; i < 6; i++) d->bars[i] = 0;

        uint8_t secondary_bus = pci_read8(bus, device, function, 0x19);
        if (secondary_bus != 0)
            pci_scan_bus(secondary_bus);

    } else {
        /* type 2 (CardBus) or unknown — no BARs we care about */
        for (int i = 0; i < 6; i++) d->bars[i] = 0;
    }

    node->next       = pci_device_list;
    pci_device_list  = node;
    pci_device_count++;
}

static void pci_scan_device(uint8_t bus, uint8_t device) {
    uint16_t vendor = pci_read16(bus, device, 0, 0x00);
    if (vendor == 0xFFFFU) return;

    uint8_t header_type = pci_read8(bus, device, 0, 0x0E);
    pci_scan_function(bus, device, 0);

    if (header_type & 0x80U) {
        for (uint8_t fn = 1; fn < 8; fn++)
            pci_scan_function(bus, device, fn);
    }
}

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++)
        pci_scan_device(bus, dev);
}

void pci_enumerate(void) {
    printk("[pci] enumerating...\n");
    pci_scan_bus(0);
    printk("[pci] found %zu devices\n", pci_device_count);
}

pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    for (pci_node_t *n = pci_device_list; n; n = n->next) {
        if (n->dev.class_code == class_code && n->dev.subclass == subclass)
            return &n->dev;
    }
    return NULL;
}

pci_device_t *pci_find_by_id(uint16_t vendor_id, uint16_t device_id) {
    for (pci_node_t *n = pci_device_list; n; n = n->next) {
        if (n->dev.vendor_id == vendor_id && n->dev.device_id == device_id)
            return &n->dev;
    }
    return NULL;
}
