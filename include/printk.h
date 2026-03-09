#pragma once

#include <stdarg.h>

/*
 * printk.h
 *
 * Minimal kernel text output to the Limine framebuffer.
 *
 * Call printk_init() once after limine_init(), then use printk() freely.
 *
 * printk() supports basic printf-style format specifiers:
 *   %d %i %u %x %X %p %s %c %%
 * Integer length modifiers: %ld/%lu and %lld/%llu.
 */

/* Initialise the console using the framebuffer in g_framebuffer.
 * Clears the screen and resets the cursor to (0, 0).                      */
void printk_init(void);

/* Write a NUL-terminated string to the framebuffer console.
 * Handles \n (newline) and \t (8-space tab stop).
 * Scrolls up by one text row when the cursor reaches the bottom.          */
void printk(const char *fmt, ...);

void clearScreen();

