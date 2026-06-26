/* bootsplash.c — animated boot screen for AuraLite OS.
 *
 * Shows immediately after gfx_init():
 *   - Gradient background
 *   - AuraLite logo (stylized 'A' / orbiting rings)
 *   - OS name + version
 *   - Animated progress bar driven by bootsplash_set_stage()
 *   - Spinning indicator + status message
 *   - Subsystem checklist
 *
 * The splash runs in the kernel framebuffer before the shell starts,
 * replacing the old serial-only boot chatter with a visual experience.
 */

#include "drivers/framebuffer/bootsplash.h"
#include "drivers/framebuffer/graphics.h"
#include "kernel/kernel.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/string.h"

static uint32_t boot_w, boot_h;
static uint32_t spinner_frame = 0;
static uint32_t current_stage = 0;
static uint32_t total_stages = 14;
static const char *current_msg = "Starting...";
static uint64_t boot_start_tick = 0;

/* Colour palette – modern dark theme with cyan accent. */
#define BS_BG_TOP    0x000a0f1a
#define BS_BG_BOTTOM 0x00141e2e
#define BS_ACCENT    0x0000d4ff
#define BS_ACCENT2   0x0000a0c0
#define BS_WHITE     0x00FFFFFF
#define BS_GRAY      0x0090a0b0
#define BS_DARK      0x00182030
#define BS_GREEN     0x0000ff88

/* Draw the AuraLite logo: three concentric rings + central A. */
static void draw_logo(uint32_t cx, uint32_t cy) {
    /* Outer glow ring */
    gfx_fill_circle(cx, cy, 56, 0x00103050);
    gfx_fill_circle(cx, cy, 52, 0x00204070);

    /* Rings */
    gfx_draw_circle(cx, cy, 48, BS_ACCENT2);
    gfx_draw_circle(cx, cy, 38, BS_ACCENT);
    gfx_draw_circle(cx, cy, 28, BS_WHITE);

    /* Central "A" – stylized triangle */
    gfx_draw_line(cx - 14, cy + 12, cx, cy - 16, BS_WHITE);
    gfx_draw_line(cx + 14, cy + 12, cx, cy - 16, BS_WHITE);
    gfx_draw_line(cx - 8, cy + 2, cx + 8, cy + 2, BS_WHITE);

    /* Orbiting dots – 4 points rotating */
    int s = (int)(spinner_frame & 31);
    /* 4 dots at 90° intervals */
    for (int i = 0; i < 4; i++) {
        int angle_idx = (s + i * 8) & 31;
        /* cheap sin/cos table: 32 steps */
        static const int8_t cos_t[32] = {
             32, 31, 30, 27, 23, 18, 12, 6,
              0, -6,-12,-18,-23,-27,-30,-31,
            -32,-31,-30,-27,-23,-18,-12,-6,
              0,  6, 12, 18, 23, 27, 30, 31
        };
        static const int8_t sin_t[32] = {
              0, 6, 12, 18, 23, 27, 30, 31,
             32, 31, 30, 27, 23, 18, 12, 6,
              0, -6,-12,-18,-23,-27,-30,-31,
            -32,-31,-30,-27,-23,-18,-12,-6
        };
        int ox = (cos_t[angle_idx] * 48) / 32;
        int oy = (sin_t[angle_idx] * 48) / 32;
        gfx_fill_circle(cx + ox, cy + oy, 3, BS_ACCENT);
    }
}

/* Render the entire splash screen. */
static void bootsplash_render(void) {
    /* Gradient background */
    gfx_gradient_v(0, 0, boot_w, boot_h, BS_BG_TOP, BS_BG_BOTTOM);

    uint32_t cx = boot_w / 2;
    uint32_t cy = boot_h / 2 - 60;

    /* Logo */
    draw_logo(cx, cy);

    /* OS name – large */
    gfx_draw_text_centered(cy + 70, "AuraLite OS", BS_WHITE);
    /* manual version string */
    const char *ver = AURALITE_NAME;
    (void)ver;
    gfx_draw_text_centered(cy + 90, "Version 1.0.0  —  x86_64", BS_GRAY);
    gfx_draw_text_centered(cy + 110, "Limine  •  SMP  •  TCP/IP  •  GUI", BS_ACCENT2);

    /* Progress bar */
    uint32_t bar_w = boot_w > 700 ? 560 : boot_w - 140;
    uint32_t bar_h = 18;
    uint32_t bar_x = (boot_w - bar_w) / 2;
    uint32_t bar_y = cy + 150;

    /* Bar background */
    gfx_fill_rect(bar_x, bar_y, bar_w, bar_h, BS_DARK);
    gfx_draw_rect(bar_x, bar_y, bar_w, bar_h, BS_GRAY);

    /* Filled portion */
    uint32_t fill = total_stages ? (current_stage * (bar_w - 4)) / total_stages : 0;
    if (fill > 0) {
        for (uint32_t i = 0; i < fill; i++) {
            uint8_t t = (uint8_t)((i * 255) / (bar_w - 4));
            color_t c = gfx_blend(BS_ACCENT2, BS_ACCENT, t);
            gfx_fill_rect(bar_x + 2 + i, bar_y + 2, 1, bar_h - 4, c);
        }
    }

    /* Percent text */
    char pct[16];
    uint32_t percent = total_stages ? (current_stage * 100) / total_stages : 0;
    /* int -> string */
    int p = 0;
    if (percent >= 100) { pct[p++] = '1'; pct[p++] = '0'; pct[p++] = '0'; }
    else if (percent >= 10) { pct[p++] = '0' + percent / 10; pct[p++] = '0' + percent % 10; }
    else { pct[p++] = '0' + percent; }
    pct[p++] = '%';
    pct[p] = 0;
    uint32_t pct_w = gfx_text_width(pct);
    gfx_draw_text(bar_x + bar_w - pct_w - 6, bar_y - 18, pct, BS_WHITE);

    /* Status message + spinner */
    const char *spinner = "|/-\\";
    char spin_msg[128];
    int sm = 0;
    spin_msg[sm++] = spinner[spinner_frame & 3];
    spin_msg[sm++] = ' ';
    spin_msg[sm++] = ' ';
    const char *m = current_msg;
    while (*m && sm < 120) spin_msg[sm++] = *m++;
    spin_msg[sm] = 0;
    gfx_draw_text_centered(bar_y + 28, spin_msg, BS_GRAY);

    /* Subsystem checklist – two columns */
    const char *checks_left[] = {
        "GDT / IDT / TSS",
        "PMM / VMM / Heap",
        "SMP / Scheduler",
        "VFS / Initrd",
        "TCP/IP / DHCP",
        "e1000 NIC",
        "USB Stack"
    };
    const char *checks_right[] = {
        "Framebuffer GUI",
        "PS/2 Keyboard",
        "Window Manager",
        "3D Renderer",
        "AHCI / BT / WiFi",
        "Ring 3 / ELF",
        "Shell Ready"
    };
    uint32_t check_y = bar_y + 60;
    uint32_t col1_x = bar_x;
    uint32_t col2_x = bar_x + bar_w / 2 + 20;
    for (int i = 0; i < 7; i++) {
        uint32_t cy_line = check_y + i * 18;
        int done = (int)current_stage > (i + (i < 7 ? 0 : 7));
        /* Simple staged activation */
        done = current_stage >= (uint32_t)(i + 1);
        color_t cc = done ? BS_GREEN : BS_GRAY;
        const char *mark = done ? "[*]" : "[ ]";
        gfx_draw_text(col1_x, cy_line, mark, cc);
        gfx_draw_text(col1_x + 32, cy_line, checks_left[i], cc);
        done = current_stage >= (uint32_t)(i + 8);
        cc = done ? BS_GREEN : BS_GRAY;
        mark = done ? "[*]" : "[ ]";
        gfx_draw_text(col2_x, cy_line, mark, cc);
        gfx_draw_text(col2_x + 32, cy_line, checks_right[i], cc);
    }

    /* Footer */
    gfx_draw_text_centered(boot_h - 40, "Booting AuraLite OS ...", BS_GRAY);
    gfx_draw_text_centered(boot_h - 20,
        "github.com/AlexanderNyr/AuraLite-OS", 0x00608090);

    gfx_flip();
}

void bootsplash_init(void) {
    boot_w = gfx_get_width();
    boot_h = gfx_get_height();
    if (boot_w == 0 || boot_h == 0) return;
    boot_start_tick = 0; /* pit_get_ticks() not exposed – use spinner_frame */
    current_stage = 0;
    total_stages = 14;
    current_msg = "Initializing graphics...";
    bootsplash_render();
}

void bootsplash_set_stage(uint32_t stage, uint32_t total, const char *message) {
    current_stage = stage;
    if (total) total_stages = total;
    if (message) current_msg = message;
    bootsplash_render();
}

void bootsplash_tick(void) {
    spinner_frame++;
    if ((spinner_frame & 3) == 0) {
        bootsplash_render();
    }
}

void bootsplash_finish(void) {
    current_stage = total_stages;
    current_msg = "Boot complete – starting shell...";
    bootsplash_render();
    /* Hold for ~1.2s with fade */
    for (volatile int f = 0; f < 6; f++) {
        spinner_frame += 4;
        bootsplash_render();
        /* ~200ms busy wait */
        for (volatile uint64_t d = 0; d < 8000000ULL; d++) { __asm__ volatile("pause"); }
    }
    /* Fade to black */
    for (int step = 0; step < 8; step++) {
        gfx_gradient_v(0, 0, boot_w, boot_h,
            gfx_blend(BS_BG_TOP, 0x00000000, (uint8_t)(step * 32)),
            gfx_blend(BS_BG_BOTTOM, 0x00000000, (uint8_t)(step * 32)));
        gfx_flip();
        for (volatile uint64_t d = 0; d < 2000000ULL; d++) { __asm__ volatile("pause"); }
    }
}
