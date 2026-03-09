
#pragma once

#include <cpuid.h>

#include <stdint.h>
#include <stdbool.h>


typedef enum {
    CPU_VENDOR_UNKNOWN = 0,
    CPU_VENDOR_INTEL,
    CPU_VENDOR_AMD,
    CPU_VENDOR_KVM,
    CPU_VENDOR_HYPERV,
    CPU_VENDOR_VMWARE,
} cpu_vendor_t;


typedef struct {
    /* Leaf 1 EDX */
    bool fpu        : 1;
    bool vme        : 1;
    bool de         : 1;
    bool pse        : 1;
    bool tsc        : 1;
    bool msr        : 1;
    bool pae        : 1;
    bool mce        : 1;
    bool cx8        : 1;
    bool apic       : 1;
    bool sep        : 1;   /* SYSENTER/SYSEXIT */
    bool mtrr       : 1;
    bool pge        : 1;
    bool mca        : 1;
    bool cmov       : 1;
    bool pat        : 1;
    bool pse36      : 1;
    bool clflush    : 1;
    bool mmx        : 1;
    bool fxsr       : 1;
    bool sse        : 1;
    bool sse2       : 1;
    bool htt        : 1;   /* Hyper-Threading */

    /* Leaf 1 ECX */
    bool sse3       : 1;
    bool pclmul     : 1;
    bool ssse3      : 1;
    bool fma        : 1;
    bool cx16       : 1;   /* CMPXCHG16B */
    bool sse4_1     : 1;
    bool sse4_2     : 1;
    bool x2apic     : 1;
    bool movbe      : 1;
    bool popcnt     : 1;
    bool aes        : 1;
    bool xsave      : 1;
    bool osxsave    : 1;
    bool avx        : 1;
    bool f16c       : 1;
    bool rdrand     : 1;
    bool hypervisor : 1;   /* Running under a hypervisor */

    /* Leaf 7 EBX (subleaf 0) */
    bool fsgsbase   : 1;
    bool smep       : 1;
    bool bmi1       : 1;
    bool avx2       : 1;
    bool bmi2       : 1;
    bool invpcid    : 1;
    bool mpx        : 1;
    bool avx512f    : 1;
    bool rdseed     : 1;
    bool adx        : 1;
    bool smap       : 1;
    bool clflushopt : 1;
    bool clwb       : 1;
    bool sha        : 1;

    /* Leaf 7 ECX (subleaf 0) */
    bool umip       : 1;
    bool pku        : 1;
    bool vaes       : 1;
    bool vpclmulqdq : 1;
    bool rdpid      : 1;

    /* Extended leaf 0x80000001 EDX */
    bool syscall    : 1;   /* SYSCALL/SYSRET */
    bool nx         : 1;   /* No-Execute */
    bool pdpe1gb    : 1;   /* 1 GB pages */
    bool rdtscp     : 1;
    bool lm         : 1;   /* Long mode (64-bit) */

    /* Extended leaf 0x80000001 ECX */
    bool lahf_lm    : 1;   /* LAHF/SAHF in 64-bit */
    bool cmp_legacy : 1;
    bool abm        : 1;   /* LZCNT */
    bool sse4a      : 1;   /* AMD only */
    bool prefetchw  : 1;
} cpu_features_t;


typedef enum {
    CACHE_TYPE_NULL = 0,
    CACHE_TYPE_DATA,
    CACHE_TYPE_INSTRUCTION,
    CACHE_TYPE_UNIFIED,
} cache_type_t;

typedef struct {
    cache_type_t type;
    uint8_t      level;        /* 1, 2, 3 */
    uint32_t     size_kb;
    uint32_t     line_size;    /* bytes */
    uint32_t     associativity;
    uint32_t     sets;
    bool         self_initializing;
    bool         fully_associative;
} cache_info_t;

#define CPUID_MAX_CACHES 8

typedef struct {
    /* Identification */
    char         vendor_string[13];  /* 12 chars + NUL */
    cpu_vendor_t vendor;
    char         brand_string[49];   /* 48 chars + NUL */

    /* Family/model/stepping (from leaf 1 EAX) */
    uint8_t      stepping;
    uint8_t      model;
    uint8_t      family;
    uint8_t      type;               /* 0=OEM, 1=OverDrive, 2=MP */
    uint8_t      extended_model;
    uint8_t      extended_family;
    uint8_t      display_model;      /* resolved: ext_model<<4 | model */
    uint16_t     display_family;     /* resolved: ext_family + family  */

    /* Topology (leaf 1 EBX) */
    uint8_t      brand_index;
    uint8_t      clflush_line_size;  /* in bytes (raw * 8) */
    uint8_t      logical_processors; /* per package */
    uint8_t      initial_apic_id;

    /* Leaves */
    uint32_t     max_basic_leaf;
    uint32_t     max_extended_leaf;

    /* Features */
    cpu_features_t features;

    /* Cache hierarchy (leaf 4 / 0x8000001D) */
    cache_info_t caches[CPUID_MAX_CACHES];
    uint8_t      cache_count;

    /* Address sizes (leaf 0x80000008) */
    uint8_t      phys_addr_bits;
    uint8_t      virt_addr_bits;
} cpu_info_t;


static inline void cpuid_get_info(cpu_info_t *info)
{
    uint32_t eax, ebx, ecx, edx;

    /* Zero everything first */
    for (uint8_t *p = (uint8_t *)info;
         p < (uint8_t *)info + sizeof(cpu_info_t); p++)
        *p = 0;

    /* ── Leaf 0: vendor + max basic leaf ─────────────────────────────── */
    __cpuid(0, eax, ebx, ecx, edx);
    info->max_basic_leaf = eax;

    uint32_t *vs = (uint32_t *)info->vendor_string;
    vs[0] = ebx;
    vs[1] = edx;
    vs[2] = ecx;
    info->vendor_string[12] = '\0';

    /* Identify vendor */
    if (__builtin_memcmp(info->vendor_string, "GenuineIntel", 12) == 0)
        info->vendor = CPU_VENDOR_INTEL;
    else if (__builtin_memcmp(info->vendor_string, "AuthenticAMD", 12) == 0)
        info->vendor = CPU_VENDOR_AMD;
    else if (__builtin_memcmp(info->vendor_string, "KVMKVMKVM\0\0\0", 12) == 0)
        info->vendor = CPU_VENDOR_KVM;
    else if (__builtin_memcmp(info->vendor_string, "Microsoft Hv", 12) == 0)
        info->vendor = CPU_VENDOR_HYPERV;
    else if (__builtin_memcmp(info->vendor_string, "VMwareVMware", 12) == 0)
        info->vendor = CPU_VENDOR_VMWARE;
    else
        info->vendor = CPU_VENDOR_UNKNOWN;

    if (info->max_basic_leaf == 0)
        return;

    __cpuid(1, eax, ebx, ecx, edx);

    info->stepping        = (eax >> 0)  & 0xF;
    info->model           = (eax >> 4)  & 0xF;
    info->family          = (eax >> 8)  & 0xF;
    info->type            = (eax >> 12) & 0x3;
    info->extended_model  = (eax >> 16) & 0xF;
    info->extended_family = (eax >> 20) & 0xFF;

    /* Resolved display values per Intel/AMD spec */
    if (info->family == 0xF)
        info->display_family = (uint16_t)(info->extended_family + info->family);
    else
        info->display_family = info->family;

    if (info->family == 0x6 || info->family == 0xF)
        info->display_model = (uint8_t)((info->extended_model << 4) | info->model);
    else
        info->display_model = info->model;

    info->brand_index        = (ebx >> 0)  & 0xFF;
    info->clflush_line_size  = (uint8_t)(((ebx >> 8) & 0xFF) * 8);
    info->logical_processors = (ebx >> 16) & 0xFF;
    info->initial_apic_id    = (ebx >> 24) & 0xFF;

    /* EDX feature flags */
    cpu_features_t *f = &info->features;
    f->fpu     = (edx >> 0)  & 1;
    f->vme     = (edx >> 1)  & 1;
    f->de      = (edx >> 2)  & 1;
    f->pse     = (edx >> 3)  & 1;
    f->tsc     = (edx >> 4)  & 1;
    f->msr     = (edx >> 5)  & 1;
    f->pae     = (edx >> 6)  & 1;
    f->mce     = (edx >> 7)  & 1;
    f->cx8     = (edx >> 8)  & 1;
    f->apic    = (edx >> 9)  & 1;
    f->sep     = (edx >> 11) & 1;
    f->mtrr    = (edx >> 12) & 1;
    f->pge     = (edx >> 13) & 1;
    f->mca     = (edx >> 14) & 1;
    f->cmov    = (edx >> 15) & 1;
    f->pat     = (edx >> 16) & 1;
    f->pse36   = (edx >> 17) & 1;
    f->clflush = (edx >> 19) & 1;
    f->mmx     = (edx >> 23) & 1;
    f->fxsr    = (edx >> 24) & 1;
    f->sse     = (edx >> 25) & 1;
    f->sse2    = (edx >> 26) & 1;
    f->htt     = (edx >> 28) & 1;

    /* ECX feature flags */
    f->sse3       = (ecx >> 0)  & 1;
    f->pclmul     = (ecx >> 1)  & 1;
    f->ssse3      = (ecx >> 9)  & 1;
    f->fma        = (ecx >> 12) & 1;
    f->cx16       = (ecx >> 13) & 1;
    f->sse4_1     = (ecx >> 19) & 1;
    f->sse4_2     = (ecx >> 20) & 1;
    f->x2apic     = (ecx >> 21) & 1;
    f->movbe      = (ecx >> 22) & 1;
    f->popcnt     = (ecx >> 23) & 1;
    f->aes        = (ecx >> 25) & 1;
    f->xsave      = (ecx >> 26) & 1;
    f->osxsave    = (ecx >> 27) & 1;
    f->avx        = (ecx >> 28) & 1;
    f->f16c       = (ecx >> 29) & 1;
    f->rdrand     = (ecx >> 30) & 1;
    f->hypervisor = (ecx >> 31) & 1;

    /* ── Leaf 4: cache topology (Intel) / 0x8000001D (AMD) ───────────── */
    /* We'll handle both; try leaf 4 first for Intel-style enumeration   */
    uint8_t ci = 0;
    if (info->max_basic_leaf >= 4) {
        for (uint32_t sub = 0; ci < CPUID_MAX_CACHES; sub++) {
            __cpuid_count(4, sub, eax, ebx, ecx, edx);
            uint8_t ctype = eax & 0x1F;
            if (ctype == 0) break; /* no more caches */

            cache_info_t *c = &info->caches[ci++];
            c->type  = (cache_type_t)ctype;
            c->level = (eax >> 5) & 0x7;
            c->self_initializing = (eax >> 8) & 1;
            c->fully_associative = (eax >> 9) & 1;

            uint32_t line  = (ebx & 0xFFF) + 1;
            uint32_t parts = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t ways  = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t sets  = ecx + 1;

            c->line_size    = line;
            c->associativity = ways;
            c->sets         = sets;
            c->size_kb      = (ways * parts * line * sets) / 1024;
        }
    }
    info->cache_count = ci;

    if (info->max_basic_leaf >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);

        f->fsgsbase   = (ebx >> 0)  & 1;
        f->smep       = (ebx >> 7)  & 1;
        f->bmi1       = (ebx >> 3)  & 1;
        f->avx2       = (ebx >> 5)  & 1;
        f->bmi2       = (ebx >> 8)  & 1;
        f->invpcid    = (ebx >> 10) & 1;
        f->mpx        = (ebx >> 14) & 1;
        f->avx512f    = (ebx >> 16) & 1;
        f->rdseed     = (ebx >> 18) & 1;
        f->adx        = (ebx >> 19) & 1;
        f->smap       = (ebx >> 20) & 1;
        f->clflushopt = (ebx >> 23) & 1;
        f->clwb       = (ebx >> 24) & 1;
        f->sha        = (ebx >> 29) & 1;

        f->umip       = (ecx >> 2)  & 1;
        f->pku        = (ecx >> 3)  & 1;
        f->vaes       = (ecx >> 9)  & 1;
        f->vpclmulqdq = (ecx >> 10) & 1;
        f->rdpid      = (ecx >> 22) & 1;
    }

    /* ── Extended leaves ─────────────────────────────────────────────── */
    __cpuid(0x80000000, eax, ebx, ecx, edx);
    info->max_extended_leaf = eax;

    if (info->max_extended_leaf < 0x80000001)
        return;

    /* Leaf 0x80000001: extended feature flags */
    __cpuid(0x80000001, eax, ebx, ecx, edx);

    f->syscall    = (edx >> 11) & 1;
    f->nx         = (edx >> 20) & 1;
    f->pdpe1gb    = (edx >> 26) & 1;
    f->rdtscp     = (edx >> 27) & 1;
    f->lm         = (edx >> 29) & 1;

    f->lahf_lm    = (ecx >> 0)  & 1;
    f->cmp_legacy = (ecx >> 1)  & 1;
    f->abm        = (ecx >> 5)  & 1;
    f->sse4a      = (ecx >> 6)  & 1;
    f->prefetchw  = (ecx >> 8)  & 1;

    /* Leaves 0x80000002–4: brand string */
    if (info->max_extended_leaf >= 0x80000004) {
        uint32_t *bp = (uint32_t *)info->brand_string;
        __cpuid(0x80000002, bp[0],  bp[1],  bp[2],  bp[3]);
        __cpuid(0x80000003, bp[4],  bp[5],  bp[6],  bp[7]);
        __cpuid(0x80000004, bp[8],  bp[9],  bp[10], bp[11]);
        info->brand_string[48] = '\0';
    }

    /* Leaf 0x80000008: address sizes */
    if (info->max_extended_leaf >= 0x80000008) {
        __cpuid(0x80000008, eax, ebx, ecx, edx);
        info->phys_addr_bits = (eax >> 0) & 0xFF;
        info->virt_addr_bits = (eax >> 8) & 0xFF;
    }

    /* AMD-specific: cache topology via 0x8000001D */
    if (info->vendor == CPU_VENDOR_AMD &&
        info->max_extended_leaf >= 0x8000001D &&
        info->cache_count == 0)
    {
        for (uint32_t sub = 0; info->cache_count < CPUID_MAX_CACHES; sub++) {
            __cpuid_count(0x8000001D, sub, eax, ebx, ecx, edx);
            uint8_t ctype = eax & 0x1F;
            if (ctype == 0) break;

            cache_info_t *c = &info->caches[info->cache_count++];
            c->type  = (cache_type_t)ctype;
            c->level = (eax >> 5) & 0x7;
            c->self_initializing = (eax >> 8) & 1;
            c->fully_associative = (eax >> 9) & 1;

            uint32_t line  = (ebx & 0xFFF) + 1;
            uint32_t parts = ((ebx >> 12) & 0x3FF) + 1;
            uint32_t ways  = ((ebx >> 22) & 0x3FF) + 1;
            uint32_t sets  = ecx + 1;

            c->line_size     = line;
            c->associativity = ways;
            c->sets          = sets;
            c->size_kb       = (ways * parts * line * sets) / 1024;
        }
    }
}

