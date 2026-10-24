/*
 * pro1x-bootmenu -- choose the system to boot, from the initramfs.
 *
 *   pro1x-bootmenu [-t seconds] [-d default-index] ENTRY...
 *
 * Draws the entries as large buttons straight onto /dev/fb0, in landscape
 * like Hildon (the panel is portrait; it is rotated to the right), and
 * prints the index of the chosen entry on stdout. Every way of choosing
 * works, so a broken part of the phone never locks one out:
 *
 *   keyboard        Up/Down or 1..9 choose, Enter starts; first letter of an
 *                   entry starts it directly
 *   side keys       Volume up/down choose, Power starts
 *   touchscreen     a tap on an entry starts it
 *   nothing at all  after the timeout the default entry starts
 *
 * Anything going wrong (no framebuffer, no font) prints the default index at
 * once, so the boot never hangs here. The font is the kernel console font,
 * read with KDFONTOP and scaled up.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 9
#define MAX_INPUTS  32
#define SCALE       4            /* 8x16 console font -> 32x64 pixels */

#define COL_BG      0x000000
#define COL_TEXT    0xffffff
#define COL_DIM     0x9a9a9a
#define COL_BUTTON  0x262626
#define COL_SELECT  0x1a7de6     /* Hildon blue */

static const char *entries[MAX_ENTRIES];
static int n_entries, selected;

/* Framebuffer: physical portrait panel, drawn as a landscape canvas. */
static uint8_t *fb;
static int fb_w, fb_h, fb_stride, fb_bpp;
static int lw, lh;               /* logical (landscape) size */

static unsigned char font[512 * 32 * 4];
static int font_w, font_h, font_bpr;

static int tty = -1;

/* Logical landscape (x, y) -> physical portrait pixel, rotation "right". */
static void put_pixel(int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= lw || y >= lh)
        return;
    int px = fb_w - 1 - y, py = x;
    uint8_t *p = fb + py * fb_stride + px * (fb_bpp / 8);
    if (fb_bpp == 32) {
        p[0] = rgb & 0xff;
        p[1] = (rgb >> 8) & 0xff;
        p[2] = (rgb >> 16) & 0xff;
        p[3] = 0xff;
    } else if (fb_bpp == 16) {
        uint16_t v = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
        memcpy(p, &v, 2);
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_pixel(xx, yy, rgb);
}

static void draw_char(int x, int y, unsigned char c, int scale, uint32_t rgb)
{
    const unsigned char *g = font + c * 32 * font_bpr;
    for (int row = 0; row < font_h; row++)
        for (int col = 0; col < font_w; col++)
            if (g[row * font_bpr + col / 8] & (0x80 >> (col % 8)))
                fill_rect(x + col * scale, y + row * scale, scale, scale, rgb);
}

static int text_width(const char *s, int scale)
{
    return (int) strlen(s) * font_w * scale;
}

static void draw_text(int x, int y, const char *s, int scale, uint32_t rgb)
{
    for (; *s; s++, x += font_w * scale)
        draw_char(x, y, (unsigned char) *s, scale, rgb);
}

/* Entry button geometry, shared by drawing and touch hit testing. */
static void button_rect(int i, int *x, int *y, int *w, int *h)
{
    int top = 200, bottom = lh - 140, gap = 24;
    int bh = (bottom - top - gap * (n_entries - 1)) / n_entries;
    if (bh > 220)
        bh = 220;
    *x = 120;
    *w = lw - 240;
    *h = bh;
    *y = top + i * (bh + gap);
}

static void draw(int seconds_left)
{
    char hint[160];

    fill_rect(0, 0, lw, lh, COL_BG);
    draw_text(120, 60, "Pro1X", SCALE, COL_TEXT);
    draw_text(120 + text_width("Pro1X  ", SCALE), 60 + (SCALE - 3) * font_h,
              "choose the system", 3, COL_DIM);

    for (int i = 0; i < n_entries; i++) {
        int x, y, w, h;
        button_rect(i, &x, &y, &w, &h);
        fill_rect(x, y, w, h, i == selected ? COL_SELECT : COL_BUTTON);
        draw_text(x + 60, y + (h - font_h * SCALE) / 2, entries[i], SCALE, COL_TEXT);
    }

    if (seconds_left >= 0)
        snprintf(hint, sizeof hint,
                 "Enter/Power or tap: start   Up/Down or Volume: choose   %d s", seconds_left);
    else
        snprintf(hint, sizeof hint, "Enter/Power or tap: start   Up/Down or Volume: choose");
    draw_text(120, lh - 100, hint, 2, COL_DIM);
}

static int setup_display(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    struct console_font_op op = { 0 };
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix))
        return -1;
    fb_w = var.xres;
    fb_h = var.yres;
    fb_bpp = var.bits_per_pixel;
    fb_stride = fix.line_length;
    if (fb_bpp != 32 && fb_bpp != 16)
        return -1;
    fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED)
        return -1;
    lw = fb_h;      /* landscape */
    lh = fb_w;

    tty = open("/dev/tty1", O_RDWR);
    if (tty < 0)
        tty = open("/dev/tty0", O_RDWR);
    if (tty < 0)
        return -1;
    op.op = KD_FONT_OP_GET;
    op.width = 32;
    op.height = 32;
    op.charcount = 512;
    op.data = font;
    if (ioctl(tty, KDFONTOP, &op) || op.width > 8 * 4)
        return -1;
    font_w = op.width;
    font_h = op.height;
    font_bpr = (font_w + 7) / 8;
    /* Keep the console from drawing kernel messages over the menu. */
    ioctl(tty, KDSETMODE, KD_GRAPHICS);
    return 0;
}

static void restore_display(void)
{
    if (tty >= 0)
        ioctl(tty, KDSETMODE, KD_TEXT);
}

/* ---- input ---------------------------------------------------------- */

struct input {
    int fd;
    char name[32];
    int is_touch;
    int min_x, max_x, min_y, max_y;
    int x, y, touching;
};
static struct input inputs[MAX_INPUTS];
static int n_inputs;

#define BITS_PER_LONG (8 * sizeof(long))
#define TEST_BIT(bit, array) ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static void scan_inputs(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) && n_inputs < MAX_INPUTS) {
        char path[300];
        unsigned long abs_bits[(ABS_MAX + BITS_PER_LONG) / BITS_PER_LONG] = { 0 };
        int known = 0;

        if (strncmp(e->d_name, "event", 5))
            continue;
        for (int i = 0; i < n_inputs; i++)
            if (!strcmp(inputs[i].name, e->d_name))
                known = 1;
        if (known)
            continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        struct input *in = &inputs[n_inputs];
        memset(in, 0, sizeof *in);
        in->fd = open(path, O_RDONLY | O_NONBLOCK);
        if (in->fd < 0)
            continue;
        snprintf(in->name, sizeof in->name, "%.31s", e->d_name);
        ioctl(in->fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits);
        if (TEST_BIT(ABS_MT_POSITION_X, abs_bits)) {
            struct input_absinfo ax, ay;
            if (!ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) &&
                !ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) &&
                ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                in->is_touch = 1;
                in->min_x = ax.minimum; in->max_x = ax.maximum;
                in->min_y = ay.minimum; in->max_y = ay.maximum;
            }
        }
        n_inputs++;
    }
    closedir(d);
}

/* Returns the chosen index, or -1 to keep waiting; may move the selection. */
static int handle_event(struct input *in, struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->value == 1) {
        switch (ev->code) {
        case KEY_UP: case KEY_VOLUMEUP: case KEY_LEFT:
            selected = (selected + n_entries - 1) % n_entries;
            return -1;
        case KEY_DOWN: case KEY_VOLUMEDOWN: case KEY_RIGHT: case KEY_TAB:
            selected = (selected + 1) % n_entries;
            return -1;
        case KEY_ENTER: case KEY_KPENTER: case KEY_POWER: case KEY_SPACE:
            return selected;
        case KEY_1 ... KEY_9:
            if (ev->code - KEY_1 < n_entries)
                return ev->code - KEY_1;
            return -1;
        default: {
            /* First letter of an entry: map the key code back to a letter. */
            static const char qwerty[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
            int idx = ev->code - KEY_Q;
            if (idx >= 0 && idx < (int) sizeof qwerty && qwerty[idx])
                for (int i = 0; i < n_entries; i++)
                    if (tolower((unsigned char) entries[i][0]) == qwerty[idx])
                        return i;
            return -1;
        }
        }
    }
    if (!in->is_touch)
        return -1;
    if (ev->type == EV_ABS && ev->code == ABS_MT_POSITION_X)
        in->x = ev->value;
    else if (ev->type == EV_ABS && ev->code == ABS_MT_POSITION_Y)
        in->y = ev->value;
    else if (ev->type == EV_ABS && ev->code == ABS_MT_TRACKING_ID)
        in->touching = ev->value >= 0;
    else if (ev->type == EV_KEY && ev->code == BTN_TOUCH)
        in->touching = ev->value;
    else if (ev->type == EV_SYN && !in->touching && (in->x || in->y)) {
        /* Finger lifted: touch -> panel pixel -> landscape coordinates. */
        int px = (in->x - in->min_x) * (fb_w - 1) / (in->max_x - in->min_x);
        int py = (in->y - in->min_y) * (fb_h - 1) / (in->max_y - in->min_y);
        int lx = py, ly = fb_w - 1 - px;
        in->x = in->y = 0;
        for (int i = 0; i < n_entries; i++) {
            int x, y, w, h;
            button_rect(i, &x, &y, &w, &h);
            if (lx >= x && lx < x + w && ly >= y && ly < y + h) {
                selected = i;
                return i;
            }
        }
    }
    return -1;
}

static int run(int timeout)
{
    time_t deadline = time(NULL) + timeout;
    int last_shown = -2;

    for (;;) {
        int left = timeout > 0 ? (int) (deadline - time(NULL)) : -1;
        struct pollfd pfd[MAX_INPUTS];
        int shown_sel = selected;

        if (timeout > 0 && left <= 0)
            return selected;
        if (left != last_shown) {
            draw(left);
            last_shown = left;
        }
        scan_inputs();      /* the touchscreen module may load late */
        for (int i = 0; i < n_inputs; i++) {
            pfd[i].fd = inputs[i].fd;
            pfd[i].events = POLLIN;
        }
        if (poll(pfd, n_inputs, 250) <= 0)
            continue;
        for (int i = 0; i < n_inputs; i++) {
            struct input_event ev;
            if (!(pfd[i].revents & POLLIN))
                continue;
            while (read(inputs[i].fd, &ev, sizeof ev) == sizeof ev) {
                int chosen = handle_event(&inputs[i], &ev);
                if (chosen >= 0)
                    return chosen;
            }
        }
        if (selected != shown_sel) {
            /* Any choice stops the countdown: the user is there. */
            timeout = 0;
            draw(-1);
            last_shown = -1;
        }
    }
}

/*
 * Test without rebooting: PRO1X_BOOTMENU_SNAPSHOT=file.ppm renders the menu
 * into memory (portrait 1080x2160, as the panel) and writes what the user
 * would see in landscape. Needs only the console font.
 */
static int snapshot(const char *path)
{
    struct console_font_op op = { 0 };
    FILE *f;

    fb_w = 1080; fb_h = 2160; fb_bpp = 32; fb_stride = fb_w * 4;
    lw = fb_h; lh = fb_w;
    fb = calloc(1, (size_t) fb_stride * fb_h);
    tty = open("/dev/tty1", O_RDWR);
    op.op = KD_FONT_OP_GET; op.width = 32; op.height = 32; op.charcount = 512; op.data = font;
    if (!fb || tty < 0 || ioctl(tty, KDFONTOP, &op))
        return 1;
    font_w = op.width; font_h = op.height; font_bpr = (font_w + 7) / 8;
    close(tty);
    tty = -1;
    draw(5);
    if (!(f = fopen(path, "wb")))
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", lw, lh);
    for (int y = 0; y < lh; y++)
        for (int x = 0; x < lw; x++) {
            uint8_t *p = fb + x * fb_stride + (fb_w - 1 - y) * 4;
            fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
        }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    int timeout = 5, opt, chosen;

    while ((opt = getopt(argc, argv, "t:d:")) != -1) {
        if (opt == 't')
            timeout = atoi(optarg);
        else if (opt == 'd')
            selected = atoi(optarg);
    }
    for (int i = optind; i < argc && n_entries < MAX_ENTRIES; i++)
        entries[n_entries++] = argv[i];
    if (selected < 0 || selected >= n_entries)
        selected = 0;
    if (getenv("PRO1X_BOOTMENU_SNAPSHOT"))
        return snapshot(getenv("PRO1X_BOOTMENU_SNAPSHOT"));
    if (n_entries < 2 || setup_display() < 0) {
        printf("%d\n", selected);
        return 0;
    }
    chosen = run(timeout);
    fill_rect(0, 0, lw, lh, COL_BG);
    restore_display();
    printf("%d\n", chosen);
    return 0;
}
