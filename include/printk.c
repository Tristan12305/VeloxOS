
#include "printk.h"
#include <boot/boot.h>
#include "font.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <include/spinlock.h>
#include <stdbool.h>

#define COLOR_FG  0x00F8F8F2   /* near-white  */
#define COLOR_BG  0x00000000   /* black       */

/* Tab width in characters.                                                */
#define TAB_WIDTH 4

static spinlock_t printlock = SPINLOCK_INIT;

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
    
    uint32_t glyph_row_pixels = FONT_HEIGHT * fb_pitch_px;

    uint32_t *dst   = fb_ptr;
    uint32_t *src   = fb_ptr + glyph_row_pixels;
    uint32_t  count = (rows_max - 1) * glyph_row_pixels;

    for (uint32_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }

    
    fill_rect(0, (rows_max - 1) * FONT_HEIGHT,
              fb_pitch_px, FONT_HEIGHT,
              COLOR_BG);
}


static void check_scroll(void) {
    if (cursor_row >= rows_max) {
        scroll_up();
        cursor_row = rows_max - 1;
    }
}

static void advance_cursor(void) {
    cursor_col++;
    if (cursor_col >= cols_max) {
        cursor_col = 0;
        cursor_row++;
        check_scroll();
    }
}

static void newline(void) {
    cursor_col = 0;
    cursor_row++;
    check_scroll();
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
    if (!str) str = "(null)";
    while (*str) emit_char(*str++);
}




static void emit_uint(uint64_t value, uint32_t base, int uppercase, int min_digits) {
    static const char digits_low[] = "0123456789abcdef";
    static const char digits_up[]  = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_up : digits_low;

    char   buf[64];
    size_t i = 0;

    if (value == 0) {
        buf[i++] = '0';
    } else {
        while (value > 0) {
            buf[i++] = digits[value % base];
            value    /= base;
        }
    }

    /* Zero-pad up to min_digits before reversing.                         */
    while ((int)i < min_digits)
        buf[i++] = '0';

    /* buf is little-endian; emit in reverse.                              */
    while (i > 0)
        emit_char(buf[--i]);
}


static void emit_int(int64_t value) {
    if (value < 0) {
        emit_char('-');
        emit_uint((uint64_t)(-(value + 1)) + 1, 10, 0, 0);
    } else {
        emit_uint((uint64_t)value, 10, 0, 0);
    }
}



typedef enum {
    LEN_NONE,
    LEN_L,
    LEN_LL,
    LEN_Z,      /* %z — size_t / ssize_t (same width as LL on x86-64)     */
} LenMod;



static void vprintk(const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit_char(*fmt);
            continue;
        }

        fmt++;   /* skip '%' */

        LenMod len = LEN_NONE;

        if (*fmt == 'z') {
            len = LEN_Z;
            fmt++;
        } else if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                len = LEN_LL;
                fmt++;
            } else {
                len = LEN_L;
            }
        }

        
        uint64_t uval = 0;
        int64_t  ival = 0;

        switch (*fmt) {
        case 'd':
        case 'i':
            if (len == LEN_LL || len == LEN_Z) {
                ival = (int64_t)va_arg(args, long long);
            } else if (len == LEN_L) {
                ival = (int64_t)va_arg(args, long);
            } else {
                ival = (int64_t)va_arg(args, int);
            }
            emit_int(ival);
            break;

        case 'u':
            if (len == LEN_LL || len == LEN_Z) {
                uval = (uint64_t)va_arg(args, unsigned long long);
            } else if (len == LEN_L) {
                uval = (uint64_t)va_arg(args, unsigned long);
            } else {
                uval = (uint64_t)va_arg(args, unsigned int);
            }
            emit_uint(uval, 10, 0, 0);
            break;

        case 'x':
            if (len == LEN_LL || len == LEN_Z) {
                uval = (uint64_t)va_arg(args, unsigned long long);
            } else if (len == LEN_L) {
                uval = (uint64_t)va_arg(args, unsigned long);
            } else {
                uval = (uint64_t)va_arg(args, unsigned int);
            }
            emit_uint(uval, 16, 0, 0);
            break;

        case 'X':
            if (len == LEN_LL || len == LEN_Z) {
                uval = (uint64_t)va_arg(args, unsigned long long);
            } else if (len == LEN_L) {
                uval = (uint64_t)va_arg(args, unsigned long);
            } else {
                uval = (uint64_t)va_arg(args, unsigned int);
            }
            emit_uint(uval, 16, 1, 0);
            break;

        case 'p':
            /* Always zero-pad to 16 hex digits so crash dumps are readable. */
            emit_string("0x");
            emit_uint((uintptr_t)va_arg(args, void *), 16, 0, 16);
            break;

        case 's':
            emit_string(va_arg(args, const char *));
            break;

        case 'c':
            emit_char((char)va_arg(args, int));
            break;

        case '%':
            emit_char('%');
            break;

        default:
            emit_char('%');
            if (len == LEN_L) {
                emit_char('l');
            } else if (len == LEN_LL) {
                emit_char('l');
                emit_char('l');
            } else if (len == LEN_Z) {
                emit_char('z');
            }
            emit_char(*fmt);
            break;
        }
    }
}



void printk_init(void) {
    
    BootFramebuffer *fb = &g_framebuffer;

    if (!fb->address)                              printk_halt();
    if (fb->bpp != 32 || fb->pitch == 0 ||
        (fb->pitch % 4) != 0)                      printk_halt();

    fb_ptr       = (uint32_t *)fb->address;
    fb_pitch_px  = (uint32_t)(fb->pitch / 4);
    fb_width_px  = (uint32_t)fb->width;
    fb_height_px = (uint32_t)fb->height;

    if (fb_width_px == 0 || fb_height_px == 0)    printk_halt();

    cols_max = fb_width_px  / FONT_WIDTH;
    rows_max = fb_height_px / FONT_HEIGHT;

    if (cols_max == 0 || rows_max == 0)            printk_halt();

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

// with spinlocks, but shouldnt be called before smp startup.

void safe_printk(const char *fmt, ...) {
    uint64_t flags = spin_lock_irqsave(&printlock);
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    spin_unlock_irqrestore(&printlock, flags);
}

void clear_screen(void) {
    fill_rect(0, 0, fb_pitch_px, fb_height_px, COLOR_BG);
    cursor_col = 0;
    cursor_row = 0;
}


void printk_hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    emit_char(digits[(value >> 4) & 0x0F]);
    emit_char(digits[value        & 0x0F]);
}

uint16_t *handoff_framebuffer(void) {
    return (uint16_t *)fb_ptr;
}
