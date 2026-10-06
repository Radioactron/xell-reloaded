#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <console/console.h>
#include <input/input.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <usb/usbmain.h>
#include <xenon_smc/xenon_smc.h>
#include <xenos/xenos.h>

#include "spooky.h"
#include "spooky_skull.h"

#define SKULL_FRAME_MS 1000
#define SKULL_REPAINT_MS 50
#define CONTROLLER_POLL_MS 10
#define RROD_BLINK_MS 500
/* Red bits 0, 2 and 3: upper-left, lower-left and lower-right. */
#define RROD_RED_MASK 0x0d

/* The same framebuffer descriptor and tiling used by libxenon's console. */
struct spooky_video_info {
    uint32_t unknown1[4];
    uint32_t base;
    uint32_t unknown2[8];
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

extern void (*stdout_hook)(const char *buf, int len);

static uint32_t *screen;
static uint32_t *saved_screen;
static size_t screen_bytes;
static unsigned int screen_width, screen_height, screen_stride;
static int skull_x, skull_y, skull_width, skull_height;
static int saved_cursor_x, saved_cursor_y;
static int initialized, usb_ready, prank, y_was_down[4];
static struct controller_data_s controller_cache[4];
static int controller_pending[4];
static int last_red_mask = -1;
static int did_poll, did_draw, polling;
static uint64_t animation_start, prank_start, last_poll, last_draw;

static struct spooky_video_info *video_info(void)
{
    return (struct spooky_video_info *)0xec806100ULL;
}

static uint32_t *video_framebuffer(const struct spooky_video_info *info)
{
    return (uint32_t *)(long)(info->base | 0x80000000);
}

static void spooky_stdout(const char *buf, int len)
{
    /* Suppress framebuffer text while the prank is visible. UART logging is
     * handled separately by libxenon and is not redirected here. */
    if (prank)
        return;
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE);
    while (len-- > 0)
        console_putch(*buf++);
}

static void install_stdout_hook(void)
{
    if (stdout_hook != spooky_stdout) {
        stdout_hook = spooky_stdout;
    }
}

static size_t pixel_offset(unsigned int x, unsigned int y)
{
    return ((y >> 5) * 32 * screen_stride + ((x >> 5) << 10)
        + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3)
        + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2);
}

static void layout_skull(void)
{
    unsigned int columns = 0, rows = 0;
    int offset_x = 0, offset_y = 0;
    int safe_width, safe_height, margin = 16;

    console_get_dimensions(&columns, &rows);
    safe_width = (int)columns * 8;
    safe_height = (int)rows * 16;
    if (xenos_is_overscan()) {
        offset_x = (int)screen_width / 28;
        offset_y = (int)screen_height / 28;
    }
    /* Leave the same overscan-safe margins as the terminal. */
    if (safe_width > (int)screen_width - offset_x * 2)
        safe_width = (int)screen_width - offset_x * 2;
    if (safe_height > (int)screen_height - offset_y * 2)
        safe_height = (int)screen_height - offset_y * 2;
    skull_width = safe_width / 7;
    if (skull_width < 112) skull_width = 112;
    if (skull_width > 176) skull_width = 176;
    if (skull_width > safe_width - margin * 2)
        skull_width = safe_width - margin * 2;
    skull_height = skull_width * SPOOKY_SKULL_HEIGHT / SPOOKY_SKULL_WIDTH;
    if (skull_height > safe_height - margin * 2) {
        skull_height = safe_height - margin * 2;
        skull_width = skull_height * SPOOKY_SKULL_WIDTH / SPOOKY_SKULL_HEIGHT;
    }
    skull_x = offset_x + safe_width - margin - skull_width;
    skull_y = offset_y + margin;
}

static int refresh_video(void)
{
    struct spooky_video_info *info = video_info();
    uint32_t *new_screen;
    unsigned int padded_height;

    if (!info->base || !info->width || !info->height ||
        info->width > 4096 || info->height > 2160)
        return 0;
    new_screen = video_framebuffer(info);
    if (new_screen == screen && info->width == screen_width &&
        info->height == screen_height)
        return 1;

    /* A kboot video-mode change invalidates old framebuffer snapshots. */
    if (prank) {
        xenon_smc_set_led(0, 0);
        prank = 0;
    }
    free(saved_screen);
    saved_screen = NULL;
    screen = new_screen;
    screen_width = info->width;
    screen_height = info->height;
    screen_stride = (screen_width + 31) & ~31u;
    padded_height = (screen_height + 31) & ~31u;
    screen_bytes = (size_t)screen_stride * padded_height * sizeof(uint32_t);
    layout_skull();
    did_draw = 0;
    return 1;
}

static void draw_skull(uint64_t now)
{
    int x, y;
    unsigned int frame;
    size_t flush_start, flush_end;

    if (!screen || skull_width <= 0 || skull_height <= 0)
        return;
    if (did_draw && tb_diff_msec(now, last_draw) < SKULL_REPAINT_MS)
        return;
    frame = (tb_diff_msec(now, animation_start) / SKULL_FRAME_MS) & 1u;
    for (y = 0; y < skull_height; ++y) {
        int sy = y * SPOOKY_SKULL_HEIGHT / skull_height;
        for (x = 0; x < skull_width; ++x) {
            int sx = x * SPOOKY_SKULL_WIDTH / skull_width;
            int pixel = sy * SPOOKY_SKULL_WIDTH + sx;
            unsigned char index = (spooky_skull_frames[frame][pixel >> 2]
                >> (6 - ((pixel & 3) * 2))) & 3;
            const unsigned char *rgb = spooky_skull_rgb[index];
            /* Black pixels erase the old jaw when switching to closed. */
            screen[pixel_offset(skull_x + x, skull_y + y)] =
                ((uint32_t)rgb[2] << 24) | ((uint32_t)rgb[1] << 16) |
                ((uint32_t)rgb[0] << 8);
        }
    }
    /* Flush only the tile rows containing the skull, not the whole screen. */
    flush_start = (size_t)(skull_y & ~31) * screen_stride * sizeof(uint32_t);
    flush_end = (size_t)((skull_y + skull_height + 31) & ~31) * screen_stride * sizeof(uint32_t);
    if (flush_end > screen_bytes) flush_end = screen_bytes;
    memdcbst((unsigned char *)screen + flush_start, (int)(flush_end - flush_start));
    last_draw = now;
    did_draw = 1;
}

void spooky_restore(void)
{
    if (!prank)
        return;
    prank = 0;
    /* Release only our LED override: let the RF board show its normal state. */
    xenon_smc_set_led(0, 0);
    last_red_mask = -1;
    if (saved_screen && screen) {
        memcpy(screen, saved_screen, screen_bytes);
        memdcbst(screen, (int)screen_bytes);
        console_set_cursor(saved_cursor_x, saved_cursor_y);
    }
    free(saved_screen);
    saved_screen = NULL;
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE);
    did_draw = 0;
}

static void toggle_prank(uint64_t now)
{
    if (prank) {
        spooky_restore();
        return;
    }
    saved_screen = malloc(screen_bytes);
    if (!saved_screen) {
        printf("\n ! Not enough memory to preserve the screen for the Y prank.\n");
        return;
    }
    memcpy(saved_screen, screen, screen_bytes);
    saved_cursor_x = console_get_cursor_x();
    saved_cursor_y = console_get_cursor_y();
    prank_start = now;
    prank = 1;
    last_red_mask = -1;
    memset(screen, 0, screen_bytes);
    memdcbst(screen, (int)screen_bytes);
    did_draw = 0;
}

void spooky_init(void)
{
    /* Initialization is also safe after a returning ELF or video reset. */
    spooky_restore();
    free(saved_screen);
    saved_screen = NULL;
    screen = NULL;
    initialized = refresh_video();
    if (!initialized) return;
    console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE);
    install_stdout_hook();
    memset(y_was_down, 0, sizeof(y_was_down));
    memset(controller_pending, 0, sizeof(controller_pending));
    animation_start = mftb();
    did_poll = 0;
    did_draw = 0;
    draw_skull(animation_start);
}

void spooky_input_ready(void)
{
    usb_ready = 1;
}

int spooky_prank_active(void)
{
    return prank;
}

void spooky_poll(void)
{
    uint64_t now;
    int port, pressed = 0;
    if (!initialized || polling) return;
    now = mftb();
    if (did_poll && tb_diff_msec(now, last_poll) < CONTROLLER_POLL_MS)
        return;
    if (!refresh_video()) return;
    install_stdout_hook();
    last_poll = now;
    did_poll = 1;
    polling = 1;
    if (usb_ready) {
        struct controller_data_s ctrl;
        usb_do_poll();
        for (port = 0; port < 4; ++port) {
            int down;
            memset(&ctrl, 0, sizeof(ctrl));
            /* No report means no change, not "released". Otherwise a held Y
             * would retrigger every time USB delivers another report. */
            if (!get_controller_data(&ctrl, port)) continue;
            controller_cache[port] = ctrl;
            controller_pending[port] = 1;
            down = ctrl.y != 0;
            if (down && !y_was_down[port]) pressed = 1;
            y_was_down[port] = down;
        }
        if (pressed) toggle_prank(now);
    }
    if (prank) {
        int mask = (tb_diff_msec(now, prank_start) / RROD_BLINK_MS) & 1u
            ? 0 : RROD_RED_MASK;
        if (mask != last_red_mask) {
            /* Only the harmless 0x99 LED command is used; no error command. */
            xenon_smc_set_led(1, mask);
            last_red_mask = mask;
        }
    }
    draw_skull(now);
    polling = 0;
}

int spooky_get_controller_data(struct controller_data_s *ctrl, int port)
{
    if (!ctrl || port < 0 || port >= 4) return 0;
    if (!initialized || !usb_ready) return get_controller_data(ctrl, port);
    spooky_poll();
    if (!controller_pending[port]) return 0;
    *ctrl = controller_cache[port];
    controller_pending[port] = 0;
    return 1;
}
