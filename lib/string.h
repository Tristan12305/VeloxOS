#pragma once
#include <stdint.h>
#include <stddef.h>


int memcmp(const void *a, const void *b, size_t n);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);