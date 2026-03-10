/* Initialisation

    Find PCI function with class code 0x01 and subclass code 0x08.
    Enable interrupts, bus-mastering DMA, and memory space access in the PCI configuration space for the function.
    Map BAR0.
    Check the controller version is supported.
    Check the capabilities register for support of the NVMe command set.
    Check the capabilities register for support of the host's page size.
    Reset the controller.
    Set the controller configuration, and admin queue base addresses.
    Start the controller.
    Enable interrupts and register a handler.
    Send the identify command to the controller. Check it is an IO controller. Record the maximum transfer size.
    Reset the software progress marker, if implemented.
    Create the first IO completion queue, and the first IO submission queue.
    Identify active namespace IDs, and then identify individual namespaces. Record their block size, capacity and whether they are read-only.

Shutdown

    Delete IO queues.
    Inform the controller of shutdown.
    Wait until CSTS.SHST updates.

Submitting a command

    Build PRP lists.
    Wait for space in the submission queue. The controller indicates its internal head pointer in completion queue entries.
    Setup the command.
    Update the queue tail doorbell register.

IRQ handler

    For each completion queue, read all entries where the phase bit has been toggled.
    Check the status of the commands.
    Use the submission queue ID and command ID to work out which submitted command corresponds to this completion entry.
    Update the completion queue head doorbell register.

*/


#include "../block/pci.h"


// NVMe is class 0x01, subclass 0x08
#define NVME_CLASS    0x01
#define NVME_SUBCLASS 0x08

nvme_init(){
    pci_device_t *dev = pci_find_by_class(NVME_CLASS, NVME_SUBCLASS);
    if (!dev) {
        printk("[nvme] no NVMe controller found\n");
        return;
    }

    printk("[nvme] found controller at %02x:%02x.%x\n",
           dev->bus, dev->device, dev->function);

    // BAR0 is the NVMe MMIO register base (64-bit BAR)
    uint64_t bar0 = dev->bars[0] & ~0xFULL;
    if ((dev->bars[0] & 0x6) == 0x4) {
        // 64-bit BAR — combine low and high halves
        bar0 |= (uint64_t)dev->bars[1] << 32;
    }

    printk("[nvme] BAR0 = 0x%016llx\n", bar0);

    // bar0 is the physical base of the NVMe controller registers (CAP, VS, CC, etc.)
    // you'll want to map this into virtual address space before dereferencing it
}
