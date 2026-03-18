#pragma once

#include <stdarg.h>
#include <stdint.h>



void printk_init(void);


void printk(const char *fmt, ...);
void safe_printk(const char *fmt, ...);
void clear_screen();

void printk_hex8(uint8_t value);

uint16_t* handoff_framebuffer();
