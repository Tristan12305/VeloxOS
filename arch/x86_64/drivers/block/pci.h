#pragma once


#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t  bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint32_t bars[6];  // raw BAR values, type-0 only
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