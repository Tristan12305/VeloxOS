#pragma once

#include <stdarg.h>
#include <stdint.h>



/* Initialise the console using the framebuffer in g_framebuffer.
 * Clears the screen and resets the cursor to (0, 0).                      */
void printk_init(void);

/* Write a NUL-terminated string to the framebuffer console.
 * Handles \n (newline) and \t (8-space tab stop).
 * Scrolls up by one text row when the cursor reaches the bottom.          */
void printk(const char *fmt, ...);

void clearScreen();

void printk_hex8(uint8_t value);

uint16_t* handoff_framebuffer();
