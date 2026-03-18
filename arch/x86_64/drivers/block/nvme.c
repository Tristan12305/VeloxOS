

#include "pci.h"
#include "nvme.h"
#include <mm/pmm.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <include/printk.h>
#include <include/spinlock.h>
#include <arch/x86_64/irq.h>
#include <arch/x86_64/cpu/apic.h>

#define NVME_CLASS      0x01
#define NVME_SUBCLASS   0x08

/* Queue depth — must be <= CAP.MQES+1 and fit in one page */
#define NVME_ADMIN_QUEUE_DEPTH  64
#define NVME_IO_QUEUE_DEPTH     64
#define NVME_ADMIN_QUEUE_ID     0
#define NVME_IO_QUEUE_ID        1
#define NVME_NAMESPACE_ID       1

#define NVME_MSIX_CAP_ID        0x11
#define NVME_MSIX_VECTOR        0x22U
#define NVME_MSIX_TABLE_ENTRY   0

/* CC register fields */
#define NVME_CC_EN              (1u << 0)
#define NVME_CC_CSS_NVM         (0u << 4)   /* NVM command set */
#define NVME_CC_MPS_4K          (0u << 7)   /* host page size = 4KB (2^(12+MPS)) */
#define NVME_CC_AMS_RR          (0u << 11)  /* round-robin arbitration */
#define NVME_CC_IOSQES          (6u << 16)  /* SQ entry = 2^6 = 64 bytes */
#define NVME_CC_IOCQES          (4u << 20)  /* CQ entry = 2^4 = 16 bytes */

/* CSTS register fields */
#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)   /* controller fatal status */

/* PCI command register bits */
#define PCI_CMD_BUS_MASTER      (1u << 2)
#define PCI_CMD_MEM_SPACE       (1u << 1)

/* CAP field extraction */
#define NVME_CAP_MQES(cap)   ((uint16_t)((cap) & 0xFFFF))           /* max queue entries (0-based) */
#define NVME_CAP_DSTRD(cap)  ((uint8_t)(((cap) >> 32) & 0xF))       /* doorbell stride exponent */
#define NVME_CAP_TO(cap)     ((uint8_t)(((cap) >> 24) & 0xFF))      /* ready timeout (x500ms) */
#define NVME_CAP_CSS(cap)    ((uint8_t)(((cap) >> 37) & 0xFF))      /* command sets supported */
#define NVME_CAP_MPSMIN(cap) ((uint8_t)(((cap) >> 48) & 0xF))       /* min page size */
#define NVME_CAP_MPSMAX(cap) ((uint8_t)(((cap) >> 52) & 0xF))       /* max page size */

/* Opcodes */
#define NVME_ADMIN_OPC_IDENTIFY      0x06
#define NVME_ADMIN_OPC_CREATE_IO_CQ  0x05
#define NVME_ADMIN_OPC_CREATE_IO_SQ  0x01

#define NVME_NVM_OPC_WRITE           0x01
#define NVME_NVM_OPC_READ            0x02

typedef struct {
    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t _res0;
    uint32_t csts;
    uint32_t _res1;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
    uint8_t  _res2[0xFC8];
    uint32_t doorbells[];
} __attribute__((packed)) nvme_bar_t;


/* 64-byte submission queue entry (generic) */
typedef struct {
    uint32_t cdw0;   /* opcode, flags, CID */
    uint32_t nsid;
    uint64_t _res;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "nvme_sqe_t must be 64 bytes");

/* 16-byte completion queue entry */
typedef struct {
    uint32_t dw0;    /* command-specific result */
    uint32_t dw1;    /* reserved */
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* bits 15:1 = status field, bit 0 = phase tag */
} __attribute__((packed)) nvme_cqe_t;

_Static_assert(sizeof(nvme_cqe_t) == 16, "nvme_cqe_t must be 16 bytes");

typedef struct {
    volatile nvme_bar_t *bar;
    uint32_t doorbell_stride; /* in bytes */

    /* Admin queues */
    nvme_sqe_t *asq;          /* virtual */
    nvme_cqe_t *acq;          /* virtual */
    uint64_t    asq_phys;
    uint64_t    acq_phys;
    uint16_t    admin_q_depth;
    uint16_t    asq_tail;
    uint16_t    acq_head;
    uint8_t     acq_phase;    /* expected phase bit */

    /* I/O queues */
    nvme_sqe_t *iosq;
    nvme_cqe_t *iocq;
    uint64_t    iosq_phys;
    uint64_t    iocq_phys;
    uint16_t    io_q_depth;
    uint16_t    iosq_tail;
    uint16_t    iosq_head;
    uint16_t    iocq_head;
    uint8_t     iocq_phase;

    uint16_t    next_admin_cid;
    uint16_t    next_io_cid;

    uint64_t    mdts_bytes;
    uint32_t    lba_size;
    uint8_t     lba_shift;

    bool        msix_enabled;
    uint16_t    msix_vector;
    bool        ready;

    spinlock_t  admin_lock;
    spinlock_t  io_lock;
} nvme_ctrl_t;

static nvme_ctrl_t g_nvme;

typedef struct {
    volatile uint8_t  in_use;
    volatile uint8_t  done;
    volatile uint16_t status;
    volatile uint32_t result;
} nvme_cmd_state_t;

static nvme_cmd_state_t g_io_cmd_state[NVME_IO_QUEUE_DEPTH];

static inline volatile uint32_t *nvme_sq_tdbl(volatile nvme_bar_t *bar, uint16_t qid, uint32_t stride) {
    return (volatile uint32_t *)((uintptr_t)bar + 0x1000 + (2 * qid) * stride);
}

static inline volatile uint32_t *nvme_cq_hdbl(volatile nvme_bar_t *bar, uint16_t qid, uint32_t stride) {
    return (volatile uint32_t *)((uintptr_t)bar + 0x1000 + (2 * qid + 1) * stride);
}


static void nvme_pci_enable(pci_device_t *dev) {
    /* Enable bus mastering and memory space decoding */
    uint16_t cmd = pci_cfg_read16(dev, 0x04);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
    pci_cfg_write16(dev, 0x04, cmd);
}

/* Spin until (reg & mask) == expected, or timeout (in rough loop iterations) */
static int nvme_poll(volatile uint32_t *reg, uint32_t mask, uint32_t expected, uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++) {
        if ((*reg & mask) == expected)
            return 0;
        /* small delay — replace with your arch delay if you have one */
        for (volatile int d = 0; d < 1000; d++);
    }
    return -1;
}

void nvme_poll_completions(void);

static inline uint64_t nvme_le64(const uint8_t *p) {
    return ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static bool nvme_get_bar_phys(const pci_device_t *dev, uint8_t bar_index, uint64_t *out_phys) {
    if (!dev || !out_phys || bar_index >= 6) {
        return false;
    }
    uint32_t bar = dev->bars[bar_index];
    if ((bar & 0x1U) != 0) {
        return false; /* I/O BAR */
    }
    uint8_t type = (bar >> 1) & 0x3U;
    if (type == 0x2U) {
        if (bar_index + 1 >= 6) {
            return false;
        }
        uint64_t high = dev->bars[bar_index + 1];
        *out_phys = (high << 32) | (bar & ~0xFULL);
    } else {
        *out_phys = (uint64_t)(bar & ~0xFULL);
    }
    return true;
}

static uint16_t nvme_calc_depth(uint16_t desired, uint16_t cap_mqes, size_t entry_size) {
    uint16_t max_hw = (uint16_t)(cap_mqes + 1);
    uint16_t depth = desired < max_hw ? desired : max_hw;
    uint16_t max_page = (uint16_t)(PMM_PAGE_SIZE / entry_size);
    if (depth > max_page) {
        depth = max_page;
    }
    if (depth < 2) {
        depth = 0;
    }
    return depth;
}

typedef struct {
    uint64_t prp1;
    uint64_t prp2;
    void    *prp_list_virt;
    uint64_t prp_list_phys;
} nvme_prp_t;

static bool nvme_build_prp(void *buf, size_t len, nvme_prp_t *out) {
    if (!buf || !out || len == 0) {
        return false;
    }

    uintptr_t vaddr = (uintptr_t)buf;
    uint64_t phys = pmm_virt_to_phys((uint64_t)vaddr);
    out->prp1 = phys;
    out->prp2 = 0;
    out->prp_list_virt = NULL;
    out->prp_list_phys = 0;

    size_t offset = vaddr & (PMM_PAGE_SIZE - 1);
    size_t first_bytes = PMM_PAGE_SIZE - offset;
    if (len <= first_bytes) {
        return true;
    }

    size_t remaining = len - first_bytes;
    size_t page_count = (remaining + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    size_t max_entries = PMM_PAGE_SIZE / sizeof(uint64_t);
    if (page_count > max_entries) {
        safe_printk("[nvme] PRP list too large (%zu pages)\n", page_count);
        return false;
    }

    void *list = pmm_alloc_page();
    if (!list) {
        return false;
    }
    memset(list, 0, PMM_PAGE_SIZE);
    uint64_t list_phys = pmm_virt_to_phys((uint64_t)(uintptr_t)list);
    uint64_t *entries = (uint64_t *)list;
    uintptr_t page_addr = vaddr + first_bytes;
    for (size_t i = 0; i < page_count; i++) {
        entries[i] = pmm_virt_to_phys((uint64_t)(uintptr_t)page_addr);
        page_addr += PMM_PAGE_SIZE;
    }

    out->prp2 = list_phys;
    out->prp_list_virt = list;
    out->prp_list_phys = list_phys;
    return true;
}

static void nvme_free_prp(nvme_prp_t *prp) {
    if (!prp) return;
    if (prp->prp_list_virt) {
        pmm_free_page(prp->prp_list_virt);
        prp->prp_list_virt = NULL;
        prp->prp_list_phys = 0;
    }
}

static bool nvme_admin_cmd(nvme_sqe_t *cmd, uint32_t *out_result, uint16_t *out_status) {
    if (!cmd || !g_nvme.bar || !g_nvme.asq || !g_nvme.acq) {
        return false;
    }

    uint64_t flags = spin_lock_irqsave(&g_nvme.admin_lock);
    uint16_t cid = g_nvme.next_admin_cid++;
    uint16_t tail = g_nvme.asq_tail;

    cmd->cdw0 |= ((uint32_t)cid << 16);
    g_nvme.asq[tail] = *cmd;
    g_nvme.asq_tail = (uint16_t)((tail + 1) % g_nvme.admin_q_depth);

    __sync_synchronize();
    *nvme_sq_tdbl(g_nvme.bar, NVME_ADMIN_QUEUE_ID, g_nvme.doorbell_stride) = g_nvme.asq_tail;

    nvme_cqe_t cqe;
    for (;;) {
        nvme_cqe_t *entry = &g_nvme.acq[g_nvme.acq_head];
        uint16_t status = entry->status;
        if ((status & 1u) != g_nvme.acq_phase) {
            __asm__ volatile("pause");
            continue;
        }
        cqe = *entry;
        g_nvme.acq_head++;
        if (g_nvme.acq_head == g_nvme.admin_q_depth) {
            g_nvme.acq_head = 0;
            g_nvme.acq_phase ^= 1u;
        }
        *nvme_cq_hdbl(g_nvme.bar, NVME_ADMIN_QUEUE_ID, g_nvme.doorbell_stride) = g_nvme.acq_head;
        break;
    }

    spin_unlock_irqrestore(&g_nvme.admin_lock, flags);

    if (out_result) {
        *out_result = cqe.dw0;
    }
    uint16_t status = (uint16_t)(cqe.status >> 1);
    if (out_status) {
        *out_status = status;
    }
    if (cqe.cid != cid) {
        safe_printk("[nvme] admin cqe cid mismatch (expected %u got %u)\n", cid, cqe.cid);
        return false;
    }
    return status == 0;
}

static bool nvme_identify_controller(uint64_t cap) {
    void *id_buf = pmm_alloc_page();
    if (!id_buf) {
        return false;
    }
    memset(id_buf, 0, PMM_PAGE_SIZE);

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_OPC_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = pmm_virt_to_phys((uint64_t)(uintptr_t)id_buf);
    cmd.cdw10 = 1; /* CNS=1 Identify Controller */

    bool ok = nvme_admin_cmd(&cmd, NULL, NULL);
    if (!ok) {
        pmm_free_page(id_buf);
        return false;
    }

    uint8_t *id = (uint8_t *)id_buf;
    uint8_t mdts = id[0x4D];
    uint8_t cntrltype = id[0x6E];

    uint8_t mpsmin = NVME_CAP_MPSMIN(cap);
    uint64_t min_page = 1ULL << (12 + mpsmin);
    if (mdts == 0) {
        g_nvme.mdts_bytes = 0;
    } else {
        uint32_t shift = (uint32_t)mdts + 12u + (uint32_t)mpsmin;
        g_nvme.mdts_bytes = (shift >= 63) ? UINT64_MAX : (1ULL << shift);
    }

    if (cntrltype != 0 && cntrltype != 1) {
        safe_printk("[nvme] unexpected controller type %u\n", cntrltype);
        pmm_free_page(id_buf);
        return false;
    }

    safe_printk("[nvme] identify ctrl: mdts=%llu bytes (min page=%llu)\n",
                (unsigned long long)g_nvme.mdts_bytes,
                (unsigned long long)min_page);

    pmm_free_page(id_buf);
    return true;
}

static bool nvme_identify_namespace(uint32_t nsid) {
    void *id_buf = pmm_alloc_page();
    if (!id_buf) {
        return false;
    }
    memset(id_buf, 0, PMM_PAGE_SIZE);

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_OPC_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = pmm_virt_to_phys((uint64_t)(uintptr_t)id_buf);
    cmd.cdw10 = 0; /* CNS=0 Identify Namespace */

    bool ok = nvme_admin_cmd(&cmd, NULL, NULL);
    if (!ok) {
        pmm_free_page(id_buf);
        return false;
    }

    uint8_t *id = (uint8_t *)id_buf;
    uint64_t nsze = nvme_le64(id);
    if (nsze == 0) {
        safe_printk("[nvme] namespace %u is empty\n", nsid);
        pmm_free_page(id_buf);
        return false;
    }

    uint8_t nlba = id[0x19];
    uint8_t flbas = id[0x1A];
    uint8_t fmt = flbas & 0x0F;
    if (fmt > nlba) {
        fmt = 0;
    }
    uint8_t lbads = id[0x80 + (fmt * 4) + 2];
    g_nvme.lba_shift = lbads;
    g_nvme.lba_size = 1u << lbads;

    safe_printk("[nvme] namespace %u: lba=%u bytes\n", nsid, g_nvme.lba_size);

    pmm_free_page(id_buf);
    return true;
}

static bool nvme_create_io_cq(uint16_t qid, uint16_t depth, bool enable_irq, uint16_t iv) {
    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_OPC_CREATE_IO_CQ;
    cmd.prp1 = g_nvme.iocq_phys;
    cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | qid;
    cmd.cdw11 = ((uint32_t)iv << 16) | ((enable_irq ? 1u : 0u) << 1) | 1u; /* IEN/PC */
    return nvme_admin_cmd(&cmd, NULL, NULL);
}

static bool nvme_create_io_sq(uint16_t qid, uint16_t depth, uint16_t cqid) {
    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_ADMIN_OPC_CREATE_IO_SQ;
    cmd.prp1 = g_nvme.iosq_phys;
    cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | qid;
    cmd.cdw11 = ((uint32_t)cqid << 16) | (0u << 1) | 1u; /* QPRIO=0, PC=1 */
    return nvme_admin_cmd(&cmd, NULL, NULL);
}

static bool nvme_setup_msix(pci_device_t *dev, uint8_t vector) {
    uint8_t cap_off = 0;
    if (!pci_find_capability(dev, NVME_MSIX_CAP_ID, &cap_off)) {
        return false;
    }

    uint16_t ctrl = pci_cfg_read16(dev, cap_off + 0x2);
    uint16_t table_size = (ctrl & 0x7FFU) + 1U;
    if (table_size <= NVME_MSIX_TABLE_ENTRY) {
        return false;
    }

    uint32_t table_info = pci_cfg_read32(dev, cap_off + 0x4);
    uint8_t bir = (uint8_t)(table_info & 0x7U);
    uint32_t table_off = table_info & ~0x7U;

    uint64_t bar_phys;
    if (!nvme_get_bar_phys(dev, bir, &bar_phys)) {
        return false;
    }

    volatile uint32_t *table = (volatile uint32_t *)(uintptr_t)pmm_phys_to_virt(bar_phys + table_off);
    volatile uint32_t *entry = table + (NVME_MSIX_TABLE_ENTRY * 4);

    ctrl |= (1u << 14); /* function mask */
    pci_cfg_write16(dev, cap_off + 0x2, ctrl);

    uint64_t msg_addr = 0xFEE00000ULL | ((uint64_t)x86_lapic_id() << 12);
    entry[0] = (uint32_t)(msg_addr & 0xFFFFFFFFU);
    entry[1] = (uint32_t)(msg_addr >> 32);
    entry[2] = (uint32_t)vector;
    entry[3] = 0; /* unmask */

    ctrl |= (1u << 15); /* enable */
    ctrl &= ~(1u << 14);
    pci_cfg_write16(dev, cap_off + 0x2, ctrl);

    g_nvme.msix_vector = NVME_MSIX_TABLE_ENTRY;
    return true;
}

static void nvme_irq_handler(interrupt_frame *frame) {
    (void)frame;
    nvme_poll_completions();
}

void nvme_poll_completions(void) {
    if (!g_nvme.iocq || !g_nvme.bar || g_nvme.io_q_depth == 0) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_nvme.io_lock);
    bool processed = false;
    for (;;) {
        nvme_cqe_t *cqe = &g_nvme.iocq[g_nvme.iocq_head];
        uint16_t status = cqe->status;
        if ((status & 1u) != g_nvme.iocq_phase) {
            break;
        }

        uint16_t cid = cqe->cid;
        if (cid < g_nvme.io_q_depth) {
            g_io_cmd_state[cid].status = (uint16_t)(status >> 1);
            g_io_cmd_state[cid].result = cqe->dw0;
            __sync_synchronize();
            g_io_cmd_state[cid].done = 1;
        }

        g_nvme.iosq_head = cqe->sq_head;
        g_nvme.iocq_head++;
        if (g_nvme.iocq_head == g_nvme.io_q_depth) {
            g_nvme.iocq_head = 0;
            g_nvme.iocq_phase ^= 1u;
        }
        processed = true;
    }

    if (processed) {
        *nvme_cq_hdbl(g_nvme.bar, NVME_IO_QUEUE_ID, g_nvme.doorbell_stride) = g_nvme.iocq_head;
    }
    spin_unlock_irqrestore(&g_nvme.io_lock, flags);
}

static bool nvme_alloc_cid(uint16_t *out_cid) {
    if (!out_cid || g_nvme.io_q_depth == 0) {
        return false;
    }
    uint16_t start = g_nvme.next_io_cid;
    for (uint16_t i = 0; i < g_nvme.io_q_depth; i++) {
        uint16_t cid = (uint16_t)((start + i) % g_nvme.io_q_depth);
        if (!g_io_cmd_state[cid].in_use) {
            g_io_cmd_state[cid].in_use = 1;
            g_io_cmd_state[cid].done = 0;
            g_io_cmd_state[cid].status = 0;
            g_io_cmd_state[cid].result = 0;
            g_nvme.next_io_cid = (uint16_t)((cid + 1) % g_nvme.io_q_depth);
            *out_cid = cid;
            return true;
        }
    }
    return false;
}

static void nvme_release_cid(uint16_t cid) {
    if (cid >= g_nvme.io_q_depth) {
        return;
    }
    g_io_cmd_state[cid].in_use = 0;
    g_io_cmd_state[cid].done = 0;
}

static bool nvme_submit_io(nvme_sqe_t *cmd, uint16_t *out_cid) {
    if (!cmd || !out_cid || !g_nvme.iosq) {
        return false;
    }

    uint64_t flags = spin_lock_irqsave(&g_nvme.io_lock);
    uint16_t cid;
    if (!nvme_alloc_cid(&cid)) {
        spin_unlock_irqrestore(&g_nvme.io_lock, flags);
        return false;
    }

    uint16_t tail = g_nvme.iosq_tail;
    uint16_t next_tail = (uint16_t)((tail + 1) % g_nvme.io_q_depth);
    if (next_tail == g_nvme.iosq_head) {
        nvme_release_cid(cid);
        spin_unlock_irqrestore(&g_nvme.io_lock, flags);
        return false;
    }

    cmd->cdw0 |= ((uint32_t)cid << 16);
    g_nvme.iosq[tail] = *cmd;
    g_nvme.iosq_tail = next_tail;

    __sync_synchronize();
    *nvme_sq_tdbl(g_nvme.bar, NVME_IO_QUEUE_ID, g_nvme.doorbell_stride) = g_nvme.iosq_tail;

    spin_unlock_irqrestore(&g_nvme.io_lock, flags);
    *out_cid = cid;
    return true;
}

static bool nvme_wait_cid(uint16_t cid, uint32_t *out_result, uint16_t *out_status) {
    while (!g_io_cmd_state[cid].done) {
        nvme_poll_completions();
        __asm__ volatile("pause");
    }

    uint32_t result = g_io_cmd_state[cid].result;
    uint16_t status = g_io_cmd_state[cid].status;

    uint64_t flags = spin_lock_irqsave(&g_nvme.io_lock);
    nvme_release_cid(cid);
    spin_unlock_irqrestore(&g_nvme.io_lock, flags);

    if (out_result) {
        *out_result = result;
    }
    if (out_status) {
        *out_status = status;
    }
    return status == 0;
}

bool nvme_init(void) {
    memset(&g_nvme, 0, sizeof(g_nvme));

    /* 1. Find the controller */
    pci_device_t *dev = pci_find_by_class(NVME_CLASS, NVME_SUBCLASS);
    if (!dev) {
        safe_printk("[nvme] no NVMe controller found\n");
        return false;
    }
    safe_printk("[nvme] found controller at %02x:%02x.%x\n",
        dev->bus, dev->device, dev->function);

    /* 2. Resolve BAR0 for MMIO */
    uint64_t phys;
    if (!nvme_get_bar_phys(dev, 0, &phys)) {
        safe_printk("[nvme] invalid BAR0\n");
        return false;
    }
    safe_printk("[nvme] BAR phys=0x%016llx\n", phys);

    /* 3. Enable PCI bus mastering */
    nvme_pci_enable(dev);

    /* 4. Map MMIO */
    volatile nvme_bar_t *bar = (volatile nvme_bar_t *)pmm_phys_to_virt(phys);
    g_nvme.bar = bar;

    /* 5. Read and log CAP */
    uint64_t cap = bar->cap;
    uint16_t mqes         = NVME_CAP_MQES(cap);   /* max queue entries supported (0-based, so +1) */
    uint8_t  dstrd        = NVME_CAP_DSTRD(cap);
    uint8_t  to           = NVME_CAP_TO(cap);     /* ready timeout in 500ms units */
    uint8_t  css          = NVME_CAP_CSS(cap);
    uint8_t  mpsmin       = NVME_CAP_MPSMIN(cap);
    uint8_t  mpsmax       = NVME_CAP_MPSMAX(cap);
    g_nvme.doorbell_stride = 4u << dstrd;

    safe_printk("[nvme] CAP=0x%016llx: MQES=%u DSTRD=%u TO=%ums MPSMIN=%u MPSMAX=%u CSS=0x%x\n",
        cap, mqes + 1, dstrd, (uint32_t)to * 500, mpsmin, mpsmax, css);

    if ((css & 0x1u) == 0) {
        safe_printk("[nvme] controller does not support NVM command set\n");
        return false;
    }
    if (mpsmin > 0) {
        safe_printk("[nvme] controller requires min page size > 4K\n");
        return false;
    }

    uint16_t admin_depth = nvme_calc_depth(NVME_ADMIN_QUEUE_DEPTH, mqes, sizeof(nvme_sqe_t));
    if (admin_depth == 0) {
        safe_printk("[nvme] admin queue depth too small\n");
        return false;
    }
    g_nvme.admin_q_depth = admin_depth;

    /* 6. Disable controller (CC.EN = 0), wait for CSTS.RDY = 0 */
    if (bar->cc & NVME_CC_EN) {
        bar->cc &= ~NVME_CC_EN;
    }
    if (nvme_poll(&bar->csts, NVME_CSTS_RDY, 0, (uint32_t)to * 500000) < 0) {
        safe_printk("[nvme] timeout waiting for controller disable\n");
        return false;
    }
    safe_printk("[nvme] controller disabled\n");

    /* 7. Allocate admin queues (physically contiguous) */
    void *asq_virt = pmm_alloc_page();
    void *acq_virt = pmm_alloc_page();
    if (!asq_virt || !acq_virt) {
        safe_printk("[nvme] admin queue alloc failed\n");
        return false;
    }
    memset(asq_virt, 0, PMM_PAGE_SIZE);
    memset(acq_virt, 0, PMM_PAGE_SIZE);

    g_nvme.asq      = (nvme_sqe_t *)asq_virt;
    g_nvme.acq      = (nvme_cqe_t *)acq_virt;
    g_nvme.asq_phys = pmm_virt_to_phys((uint64_t)(uintptr_t)asq_virt);
    g_nvme.acq_phys = pmm_virt_to_phys((uint64_t)(uintptr_t)acq_virt);
    g_nvme.asq_tail = 0;
    g_nvme.acq_head = 0;
    g_nvme.acq_phase = 1;

    /* 8. Set AQA: admin SQ size and CQ size (0-based) */
    uint32_t aqa = ((uint32_t)(admin_depth - 1) << 16) | (uint32_t)(admin_depth - 1);
    bar->aqa = aqa;

    /* 9. Write ASQ and ACQ physical base addresses */
    bar->asq = g_nvme.asq_phys;
    bar->acq = g_nvme.acq_phys;

    /* 10. Configure CC and enable */
    bar->cc = NVME_CC_CSS_NVM
            | NVME_CC_MPS_4K
            | NVME_CC_AMS_RR
            | NVME_CC_IOSQES
            | NVME_CC_IOCQES
            | NVME_CC_EN;

    /* 11. Wait for CSTS.RDY = 1 */
    if (nvme_poll(&bar->csts, NVME_CSTS_RDY, NVME_CSTS_RDY, (uint32_t)to * 500000) < 0) {
        safe_printk("[nvme] timeout waiting for controller ready\n");
        return false;
    }
    if (bar->csts & NVME_CSTS_CFS) {
        safe_printk("[nvme] controller fatal status after enable\n");
        return false;
    }

    safe_printk("[nvme] controller ready, admin queue depth=%u, doorbell stride=%u\n",
        admin_depth, g_nvme.doorbell_stride);

    if (!nvme_identify_controller(cap)) {
        safe_printk("[nvme] identify controller failed\n");
        return false;
    }
    if (!nvme_identify_namespace(NVME_NAMESPACE_ID)) {
        safe_printk("[nvme] identify namespace failed\n");
        return false;
    }

    /* MSI-X setup (optional) */
    bool irq_ok = irq_register_handler(NVME_MSIX_VECTOR, nvme_irq_handler);
    if (irq_ok && nvme_setup_msix(dev, NVME_MSIX_VECTOR)) {
        g_nvme.msix_enabled = true;
        safe_printk("[nvme] MSI-X enabled (vector=0x%02x, table=%u)\n",
                    NVME_MSIX_VECTOR, (unsigned)g_nvme.msix_vector);
    } else {
        if (irq_ok) {
            irq_unregister_handler(NVME_MSIX_VECTOR);
        }
        g_nvme.msix_enabled = false;
    }

    uint16_t io_depth = NVME_IO_QUEUE_DEPTH;
    uint16_t max_hw = (uint16_t)(mqes + 1);
    uint16_t max_sq = (uint16_t)(PMM_PAGE_SIZE / sizeof(nvme_sqe_t));
    uint16_t max_cq = (uint16_t)(PMM_PAGE_SIZE / sizeof(nvme_cqe_t));
    if (io_depth > max_hw) io_depth = max_hw;
    if (io_depth > max_sq) io_depth = max_sq;
    if (io_depth > max_cq) io_depth = max_cq;
    if (io_depth < 2) {
        safe_printk("[nvme] IO queue depth too small\n");
        return false;
    }
    g_nvme.io_q_depth = io_depth;

    void *iosq_virt = pmm_alloc_page();
    void *iocq_virt = pmm_alloc_page();
    if (!iosq_virt || !iocq_virt) {
        safe_printk("[nvme] IO queue alloc failed\n");
        return false;
    }
    memset(iosq_virt, 0, PMM_PAGE_SIZE);
    memset(iocq_virt, 0, PMM_PAGE_SIZE);

    g_nvme.iosq      = (nvme_sqe_t *)iosq_virt;
    g_nvme.iocq      = (nvme_cqe_t *)iocq_virt;
    g_nvme.iosq_phys = pmm_virt_to_phys((uint64_t)(uintptr_t)iosq_virt);
    g_nvme.iocq_phys = pmm_virt_to_phys((uint64_t)(uintptr_t)iocq_virt);
    g_nvme.iosq_tail = 0;
    g_nvme.iosq_head = 0;
    g_nvme.iocq_head = 0;
    g_nvme.iocq_phase = 1;
    memset(g_io_cmd_state, 0, sizeof(g_io_cmd_state));

    if (!nvme_create_io_cq(NVME_IO_QUEUE_ID, io_depth, g_nvme.msix_enabled, g_nvme.msix_vector)) {
        safe_printk("[nvme] create IO CQ failed\n");
        return false;
    }
    if (!nvme_create_io_sq(NVME_IO_QUEUE_ID, io_depth, NVME_IO_QUEUE_ID)) {
        safe_printk("[nvme] create IO SQ failed\n");
        return false;
    }

    g_nvme.ready = true;
    safe_printk("[nvme] IO queues ready (depth=%u)\n", io_depth);
    return true;
}

bool nvme_read(uint64_t sector, uint32_t count, void *buf) {
    if (!g_nvme.ready || !buf || count == 0) {
        return false;
    }
    if (count > 0x10000U) {
        return false;
    }

    uint64_t bytes = (uint64_t)count * (uint64_t)g_nvme.lba_size;
    if (g_nvme.lba_size == 0 || (bytes / g_nvme.lba_size) != count) {
        return false;
    }
    if (g_nvme.mdts_bytes != 0 && bytes > g_nvme.mdts_bytes) {
        safe_printk("[nvme] read exceeds MDTS (%llu > %llu)\n",
                    (unsigned long long)bytes,
                    (unsigned long long)g_nvme.mdts_bytes);
        return false;
    }

    nvme_prp_t prp;
    if (!nvme_build_prp(buf, (size_t)bytes, &prp)) {
        return false;
    }

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_NVM_OPC_READ;
    cmd.nsid = NVME_NAMESPACE_ID;
    cmd.prp1 = prp.prp1;
    cmd.prp2 = prp.prp2;
    cmd.cdw10 = (uint32_t)(sector & 0xFFFFFFFFULL);
    cmd.cdw11 = (uint32_t)(sector >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFFu;

    uint16_t cid;
    if (!nvme_submit_io(&cmd, &cid)) {
        nvme_free_prp(&prp);
        return false;
    }

    uint16_t status;
    bool ok = nvme_wait_cid(cid, NULL, &status);
    nvme_free_prp(&prp);
    return ok;
}

bool nvme_write(uint64_t sector, uint32_t count, void *buf) {
    if (!g_nvme.ready || !buf || count == 0) {
        return false;
    }
    if (count > 0x10000U) {
        return false;
    }

    uint64_t bytes = (uint64_t)count * (uint64_t)g_nvme.lba_size;
    if (g_nvme.lba_size == 0 || (bytes / g_nvme.lba_size) != count) {
        return false;
    }
    if (g_nvme.mdts_bytes != 0 && bytes > g_nvme.mdts_bytes) {
        safe_printk("[nvme] write exceeds MDTS (%llu > %llu)\n",
                    (unsigned long long)bytes,
                    (unsigned long long)g_nvme.mdts_bytes);
        return false;
    }

    nvme_prp_t prp;
    if (!nvme_build_prp(buf, (size_t)bytes, &prp)) {
        return false;
    }

    nvme_sqe_t cmd = {0};
    cmd.cdw0 = NVME_NVM_OPC_WRITE;
    cmd.nsid = NVME_NAMESPACE_ID;
    cmd.prp1 = prp.prp1;
    cmd.prp2 = prp.prp2;
    cmd.cdw10 = (uint32_t)(sector & 0xFFFFFFFFULL);
    cmd.cdw11 = (uint32_t)(sector >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFFu;

    uint16_t cid;
    if (!nvme_submit_io(&cmd, &cid)) {
        nvme_free_prp(&prp);
        return false;
    }

    uint16_t status;
    bool ok = nvme_wait_cid(cid, NULL, &status);
    nvme_free_prp(&prp);
    return ok;
}
