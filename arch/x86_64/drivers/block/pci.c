#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <mm/vmalloc.h>
#include "pci.h"
#include <include/printk.h>
#include <arch/x86_64/cpu/vendor.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC


#define PCI_MAX_DEVICES 256


static inline void outl_u32(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint32_t inl_u32(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1U << 31) |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) |
                       (offset & 0xFCU);

    outl_u32(PCI_CONFIG_ADDRESS, address);
    return inl_u32(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, device, function, offset);
    uint8_t shift = (offset & 2U) * 8U;
    return (uint16_t)((value >> shift) & 0xFFFFU);
}

static uint8_t pci_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, device, function, offset);
    uint8_t shift = (offset & 3U) * 8U;
    return (uint8_t)((value >> shift) & 0xFFU);
}



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
    d->bus        = bus;
    d->device     = device;
    d->function   = function;
    d->vendor_id  = vendor;
    d->device_id  = pci_read16(bus, device, function, 0x02);
    d->class_code = pci_read8 (bus, device, function, 0x0B);
    d->subclass   = pci_read8 (bus, device, function, 0x0A);
    d->prog_if    = pci_read8 (bus, device, function, 0x09);
    d->revision   = pci_read8 (bus, device, function, 0x08);
    d->header_type= pci_read8 (bus, device, function, 0x0E);

    // Read BARs only for type-0 (endpoint) headers
    if ((d->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++) {
            d->bars[i] = pci_read32(bus, device, function, 0x10 + i * 4);
        }
    } else {
        for (int i = 0; i < 6; i++) d->bars[i] = 0;
    }

    // Prepend to list (order doesn't matter at enumeration time)
    node->next = pci_device_list;
    pci_device_list = node;
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

void pci_enumerate(void) {
    printk("[pci] enumerating...\n");
    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            pci_scan_device((uint8_t)bus, dev);
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