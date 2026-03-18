#pragma once


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t  bus, device, function;
    uint16_t vendor_id, device_id;
    uint16_t command, status;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_pin, irq_line;
    uint32_t bars[6];  // raw BAR values
} pci_device_t;

typedef struct pci_node {
    pci_device_t        dev;
    struct pci_node    *next;
} pci_node_t;

extern pci_node_t *pci_device_list;
extern size_t      pci_device_count;

void pci_enumerate(void);
pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass);
pci_device_t *pci_find_by_id(uint16_t vendor_id, uint16_t device_id);

uint32_t pci_cfg_read32(const pci_device_t *dev, uint8_t offset);
uint16_t pci_cfg_read16(const pci_device_t *dev, uint8_t offset);
uint8_t  pci_cfg_read8 (const pci_device_t *dev, uint8_t offset);

void pci_cfg_write32(const pci_device_t *dev, uint8_t offset, uint32_t value);
void pci_cfg_write16(const pci_device_t *dev, uint8_t offset, uint16_t value);
void pci_cfg_write8 (const pci_device_t *dev, uint8_t offset, uint8_t value);

bool pci_find_capability(const pci_device_t *dev, uint8_t cap_id, uint8_t *out_offset);
