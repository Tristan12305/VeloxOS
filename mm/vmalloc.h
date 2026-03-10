#pragma once

#include <stddef.h>
#include <stdint.h>

#include "paging.h"

#define VMALLOC_DEFAULT_FLAGS (PAGING_FLAG_WRITABLE | PAGING_FLAG_GLOBAL)

void vmalloc_init(void);
void* vmalloc(size_t size, uint64_t flags);
void vfree(void* addr, size_t size);
