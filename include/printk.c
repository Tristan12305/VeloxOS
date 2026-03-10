/*
 * printk.c
 *
 * Framebuffer text console backed by the 8x16 bitmap font in font.h.
 *
 * Coordinate conventions
 * ----------------------
 *   cursor_col  - current column  in units of glyphs (0 .. cols_max-1)
 *   cursor_row  - current row     in units of glyphs (0 .. rows_max-1)
 *   All pixel arithmetic converts via FONT_WIDTH / FONT_HEIGHT.
 *
 * Pixel format
 * ------------
 *   Limine almost always gives us a 32-bpp BGRX or RGBX framebuffer.
 *   We write 0x00RRGGBB packed into a uint32_t; swap R/B if your screen
 *   shows inverted colours.
 *
 * Scrolling
 * ---------
 *   When the cursor moves past the last row we memmove the entire pixel
 *   buffer up by one glyph row (FONT_HEIGHT * pitch bytes) and clear the
 *   newly exposed bottom row.
 */

#include "printk.h"
#include <boot/boot.h>
#include "font.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * Compile-time tunables
 * ----------------------------------------------------------------------- */

/* Foreground / background colours as 0x00RRGGBB.
 * Swap to taste or make runtime-configurable later.                       */
#define COLOR_FG  0x00F8F8F2   /* near-white  */
#define COLOR_BG  0x00000000   /* black       */

/* Tab width in characters.                                                */
#define TAB_WIDTH 4

/* -----------------------------------------------------------------------
 * Internal state
 * ----------------------------------------------------------------------- */

static uint32_t *fb_ptr;       /* Base of the framebuffer as 32-bpp words  */
static uint32_t  fb_pitch_px;  /* Pitch in 32-bpp words (pixels per row)   */
static uint32_t  fb_width_px;  /* Width  in pixels                         */
static uint32_t  fb_height_px; /* Height in pixels                         */

static uint32_t  cols_max;     /* Number of glyph columns that fit         */
static uint32_t  rows_max;     /* Number of glyph rows    that fit         */

static uint32_t  cursor_col;
static uint32_t  cursor_row;

static void printk_halt(void) {
    __asm__ volatile (
        "cli\n"
        "1: hlt\n"
        "jmp 1b\n"
    );
    __builtin_unreachable();
}



static inline void put_pixel(uint32_t x, uint32_t y, uint32_t colour) {
    fb_ptr[y * fb_pitch_px + x] = colour;
}



static void fill_rect(uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h,
                      uint32_t colour) {
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *line = fb_ptr + (y + row) * fb_pitch_px + x;
        for (uint32_t col = 0; col < w; col++) {
            line[col] = colour;
        }
    }
}



static void draw_glyph(uint32_t gcol, uint32_t grow, unsigned char c) {
    /* Clamp to printable range; render a space for anything outside.      */
    if (c >= FONT_GLYPHS) c = ' ';

    const uint8_t *bitmap = font_bitmap[c];
    uint32_t px = gcol * FONT_WIDTH;
    uint32_t py = grow * FONT_HEIGHT;

    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = bitmap[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            /* MSB of each byte is the leftmost pixel.                     */
            uint32_t colour = (bits & (0x80u >> col)) ? COLOR_FG : COLOR_BG;
            put_pixel(px + col, py + row, colour);
        }
    }
}



static void scroll_up(void) {
    /*
     * Move every pixel row above the first glyph row upward by
     * FONT_HEIGHT rows.  pitch is in bytes in the Limine struct but we
     * stored fb_pitch_px in pixels; convert carefully.
     */
    uint32_t glyph_row_pixels = FONT_HEIGHT * fb_pitch_px; /* pixels to skip */

    /* Source: start of second glyph row; Destination: start of framebuffer */
    uint32_t *dst = fb_ptr;
    uint32_t *src = fb_ptr + glyph_row_pixels;
    uint32_t  count = (rows_max - 1) * glyph_row_pixels; /* pixels to copy  */

    /* Manual memmove — no libc available yet.                             */
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }

    /* Clear the last glyph row (full pitch width).                        */
    fill_rect(0, (rows_max - 1) * FONT_HEIGHT,
              fb_pitch_px, FONT_HEIGHT,
              COLOR_BG);
}



static void advance_cursor(void) {
    cursor_col++;
    if (cursor_col >= cols_max) {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= rows_max) {
            scroll_up();
            cursor_row = rows_max - 1;
        }
    }
}

static void newline(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= rows_max) {
        scroll_up();
        cursor_row = rows_max - 1;
    }
}

static void emit_char(char c) {
    switch (c) {
    case '\n':
        newline();
        break;

    case '\r':
        cursor_col = 0;
        break;

    case '\t': {
        uint32_t spaces = TAB_WIDTH - (cursor_col % TAB_WIDTH);
        for (uint32_t i = 0; i < spaces; i++) {
            draw_glyph(cursor_col, cursor_row, ' ');
            advance_cursor();
        }
        break;
    }

    default:
        draw_glyph(cursor_col, cursor_row, (unsigned char)c);
        advance_cursor();
        break;
    }
}

static void emit_string(const char *str) {
    if (!str) {
        str = "(null)";
    }

    while (*str) {
        emit_char(*str++);
    }
}

static void emit_uint(uint64_t value, uint32_t base, int uppercase) {
    static const char digits_low[] = "0123456789abcdef";
    static const char digits_up[]  = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_up : digits_low;
    char buf[32];
    size_t i = 0;

    if (base < 2 || base > 16) {
        return;
    }

    if (value == 0) {
        emit_char('0');
        return;
    }

    while (value != 0) {
        buf[i++] = digits[value % base];
        value /= base;
    }

    while (i > 0) {
        emit_char(buf[--i]);
    }
}

static void emit_int(int64_t value) {
    uint64_t magnitude;

    if (value < 0) {
        emit_char('-');
        magnitude = (uint64_t)(-(value + 1)) + 1;
    } else {
        magnitude = (uint64_t)value;
    }

    emit_uint(magnitude, 10, 0);
}

static void vprintk(const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit_char(*fmt);
            continue;
        }

        fmt++;
        if (*fmt == '\0') {
            emit_char('%');
            break;
        }

        if (*fmt == '%') {
            emit_char('%');
            continue;
        }

        enum { LEN_NONE, LEN_L, LEN_LL } len = LEN_NONE;
        if (*fmt == 'l') {
            if (*(fmt + 1) == 'l') {
                len = LEN_LL;
                fmt++;
            } else {
                len = LEN_L;
            }
            fmt++;
            if (*fmt == '\0') {
                emit_char('%');
                emit_char('l');
                if (len == LEN_LL) {
                    emit_char('l');
                }
                break;
            }
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            if (len == LEN_LL) {
                emit_int(va_arg(args, long long));
            } else if (len == LEN_L) {
                emit_int(va_arg(args, long));
            } else {
                emit_int(va_arg(args, int));
            }
            break;

        case 'u':
            if (len == LEN_LL) {
                emit_uint(va_arg(args, unsigned long long), 10, 0);
            } else if (len == LEN_L) {
                emit_uint(va_arg(args, unsigned long), 10, 0);
            } else {
                emit_uint(va_arg(args, unsigned int), 10, 0);
            }
            break;

        case 'x':
            if (len == LEN_LL) {
                emit_uint(va_arg(args, unsigned long long), 16, 0);
            } else if (len == LEN_L) {
                emit_uint(va_arg(args, unsigned long), 16, 0);
            } else {
                emit_uint(va_arg(args, unsigned int), 16, 0);
            }
            break;

        case 'X':
            if (len == LEN_LL) {
                emit_uint(va_arg(args, unsigned long long), 16, 1);
            } else if (len == LEN_L) {
                emit_uint(va_arg(args, unsigned long), 16, 1);
            } else {
                emit_uint(va_arg(args, unsigned int), 16, 1);
            }
            break;

        case 'p':
            emit_string("0x");
            emit_uint((uintptr_t)va_arg(args, void *), 16, 0);
            break;

        case 's':
            emit_string(va_arg(args, const char *));
            break;

        case 'c':
            emit_char((char)va_arg(args, int));
            break;

        default:
            emit_char('%');
            if (len == LEN_L) {
                emit_char('l');
            } else if (len == LEN_LL) {
                emit_char('l');
                emit_char('l');
            }
            emit_char(*fmt);
            break;
        }
    }
}


void printk_init(void) {
    BootFramebuffer *fb = &g_framebuffer;

    if (!fb->address) {
        printk_halt();
    }

    if (fb->bpp != 32 || fb->pitch == 0 || (fb->pitch % 4) != 0) {
        printk_halt();
    }

    fb_ptr       = (uint32_t *)fb->address;
    fb_pitch_px  = (uint32_t)(fb->pitch / 4);
    fb_width_px  = (uint32_t)fb->width;
    fb_height_px = (uint32_t)fb->height;

    if (fb_width_px == 0 || fb_height_px == 0) {
        printk_halt();
    }

    cols_max = fb_width_px  / FONT_WIDTH;
    rows_max = fb_height_px / FONT_HEIGHT;

    if (cols_max == 0 || rows_max == 0) {
        printk_halt();
    }

    cursor_col = 0;
    cursor_row = 0;

    fill_rect(0, 0, fb_pitch_px, fb_height_px, COLOR_BG);
}

void printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}


void clearScreen(){
    fill_rect(0, 0, fb_pitch_px, fb_height_px, COLOR_BG);
    cursor_col = 0;
    cursor_row = 0;
}


void printk_hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char hi = digits[(value >> 4) & 0x0F];
    char lo = digits[value & 0x0F];
    printk("%c%c", hi, lo);
}