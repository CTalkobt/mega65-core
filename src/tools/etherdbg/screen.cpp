/*
 * screen.cpp - MEGA65 screen capture via Ethernet debug
 *
 * Ported from mega65-tools/src/tools/screen_shot.c
 * Copyright (C) 2014-2020 Paul Gardner-Stephen (original)
 *
 * Uses etherdbg transport abstraction instead of serial monitor.
 */

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <print>

#ifdef HAS_LIBPNG
#define PNG_DEBUG 3
#include <png.h>
#endif

#include "screen.h"
#include "commands.h"

namespace etherdbg {

namespace {

constexpr int SCREEN_POSITION = (800 - 720) / 2;
constexpr int MAX_SCREEN_SIZE = 128 * 1024;

uint8_t mega65_rgb(const VideoState& s, int colour, int rgb, bool alt)
{
    int offset = (alt ? 0x400 : 0x100) + (0x100 * rgb) + colour;
    if (offset >= static_cast<int>(s.vic_regs.size()))
        return 0;
    return ((s.vic_regs[offset] & 0xf) << 4)
         | ((s.vic_regs[offset] & 0xf0) >> 4);
}

// ---------------------------------------------------------------------------
// UTF-8 helpers for screencode display (from m65 screen_shot.c)
// ---------------------------------------------------------------------------

struct Utf { char mask; char lead; uint32_t beg; uint32_t end; int bits; };

static const Utf utf_table[] = {
    { 0b00111111, (char)0b10000000, 0,      0,       6 },
    { 0b01111111, 0b00000000,       0000,   0177,    7 },
    { 0b00011111, (char)0b11000000, 0200,   03777,   5 },
    { 0b00001111, (char)0b11100000, 04000,  0177777, 4 },
    { 0b00000111, (char)0b11110000, 0200000,04177777,3 },
};
static constexpr int UTF_TABLE_SIZE = 5;

int codepoint_len(uint32_t cp) {
    for (int i = 1; i < UTF_TABLE_SIZE; i++)
        if (cp >= utf_table[i].beg && cp <= utf_table[i].end)
            return i;
    return 1;
}

std::string to_utf8(uint32_t cp) {
    int bytes = codepoint_len(cp);
    std::string ret(bytes, '\0');
    int shift = utf_table[0].bits * (bytes - 1);
    ret[0] = static_cast<char>((cp >> shift & utf_table[bytes].mask) | utf_table[bytes].lead);
    shift -= utf_table[0].bits;
    for (int i = 1; i < bytes; i++) {
        ret[i] = static_cast<char>((cp >> shift & utf_table[0].mask) | utf_table[0].lead);
        shift -= utf_table[0].bits;
    }
    return ret;
}

void print_screencode(uint8_t c, bool upper_case)
{
    static const int map[][2] = {
        { 0x40, 0x2501 }, { 0x43, 0x2501 }, { 0x60, 0xa0 },
        { 0x61, 0x258c }, { 0x62, 0x2584 }, { 0x63, 0x2594 },
        { 0x64, 0x2581 }, { 0x65, 0x258e }, { 0x66, 0x2592 },
        { 0x67, 0x258a }, { 0x68, 0x25db }, { 0x69, 0x25e4 },
        { 0x6a, 0x258a }, { 0x6b, 0x2523 }, { 0x6c, 0x2597 },
        { 0x6d, 0x2517 }, { 0x6e, 0x2513 }, { 0x6f, 0x2582 },
        { -1, -1 }
    };
    bool rev = false;
    if (c & 0x80) {
        rev = true;
        c &= 0x7f;
        std::printf("\033[7m");
    }
    if (c >= '0' && c <= '9')
        std::printf("%c", c);
    else if (c <= 0x1f) {
        std::printf("%c", upper_case ? c + 0x40 : c + 0x60);
    }
    else if (c >= 0x20 && c < 0x40)
        std::printf("%c", c);
    else if (c >= 0x40 && c <= 0x5f && !upper_case)
        std::printf("%c", c);
    else {
        bool found = false;
        for (int k = 0; map[k][0] != -1; k++) {
            if (c == map[k][0]) {
                auto u = to_utf8(map[k][1]);
                std::printf("%s", u.c_str());
                found = true;
                break;
            }
        }
        if (!found)
            std::printf("?");
    }
    if (rev)
        std::printf("\033[0m");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// get_video_state
// ---------------------------------------------------------------------------

bool get_video_state(Transport& transport, VideoState& s, bool verbose)
{
    if (verbose)
        std::println("Fetching VIC-IV registers...");

    /* Read VIC-IV registers: $FFD3000, 256 bytes */
    s.vic_regs.resize(0x700, 0);
    auto regs = cmd_read_memory(transport, 0xffd3000, 0x100, false);
    if (regs.size() < 0x100) {
        std::println(stderr, "etherdbg: failed to read VIC-IV registers");
        return false;
    }
    std::copy(regs.begin(), regs.end(), s.vic_regs.begin());

    /* Read palette data: need to handle palette selection */
    uint8_t palreg = s.vic_regs[0x70];
    uint8_t btpalsel = (palreg & 0x30) >> 4;
    uint8_t altpalsel = palreg & 0x3;
    uint8_t mapedpal = palreg >> 6;

    /* If mapped palette doesn't match BT palette, switch before reading */
    if (mapedpal != btpalsel) {
        uint8_t mapbtpal = (palreg & 0x3f) | (btpalsel << 6);
        cmd_write_memory(transport, 0xffd3070, {&mapbtpal, 1}, false);
    }

    /* Read BT palette: $FFD3100, 768 bytes (R/G/B × 256) */
    auto pal = cmd_read_memory(transport, 0xffd3100, 0x300, false);
    if (pal.size() >= 0x300)
        std::copy(pal.begin(), pal.end(), s.vic_regs.begin() + 0x100);

    if (btpalsel != altpalsel) {
        /* Fetch alternate palette */
        uint8_t mapaltpal = (palreg & 0x3f) | (altpalsel << 6);
        cmd_write_memory(transport, 0xffd3070, {&mapaltpal, 1}, false);
        auto altpal = cmd_read_memory(transport, 0xffd3100, 0x300, false);
        if (altpal.size() >= 0x300)
            std::copy(altpal.begin(), altpal.end(), s.vic_regs.begin() + 0x400);
    }
    else {
        std::copy(s.vic_regs.begin() + 0x100,
                  s.vic_regs.begin() + 0x400,
                  s.vic_regs.begin() + 0x400);
    }

    /* Restore palette register if we changed it */
    if (mapedpal != btpalsel || mapedpal != altpalsel)
        cmd_write_memory(transport, 0xffd3070, {&palreg, 1}, false);

    /* Derive video state from registers */
    s.screen_address = s.vic_regs[0x60]
                     | (s.vic_regs[0x61] << 8)
                     | (s.vic_regs[0x62] << 16);
    s.charset_address = s.vic_regs[0x68]
                      | (s.vic_regs[0x69] << 8)
                      | (s.vic_regs[0x6A] << 16);
    /* Handle C64 ROM charset mappings */
    if (s.charset_address == 0x1000) s.charset_address = 0x2D000;
    if (s.charset_address == 0x9000) s.charset_address = 0x29000;
    if (s.charset_address == 0x1800) s.charset_address = 0x2D800;
    if (s.charset_address == 0x9800) s.charset_address = 0x29800;

    s.is_pal_mode = (s.vic_regs[0x6f] & 0x80) == 0;
    s.screen_line_step = s.vic_regs[0x58] | (s.vic_regs[0x59] << 8);
    s.colour_address = s.vic_regs[0x64] | (s.vic_regs[0x65] << 8);
    s.screen_width = s.vic_regs[0x5e];
    s.upper_case = (s.vic_regs[0x18] & 2) == 0;
    s.screen_rows = 1 + s.vic_regs[0x7B];
    s.sixteenbit_mode = (s.vic_regs[0x54] & 1) != 0;
    s.screen_size = s.screen_line_step * s.screen_rows * (1 + s.sixteenbit_mode);
    s.charset_size = 2048;
    s.extended_background_mode = (s.vic_regs[0x11] & 0x40) != 0;
    s.multicolour_mode = (s.vic_regs[0x16] & 0x10) != 0;
    s.bitmap_mode = (s.vic_regs[0x11] & 0x20) != 0;

    s.border_colour = s.vic_regs[0x20];
    s.background_colour = s.vic_regs[0x21];

    s.current_physical_raster = s.vic_regs[0x52] | ((s.vic_regs[0x53] & 0x3) << 8);
    s.next_raster_interrupt = s.vic_regs[0x79] | ((s.vic_regs[0x7A] & 0x3) << 8);
    if (!(s.vic_regs[0x53] & 0x80)) s.current_physical_raster *= 2;
    if (!(s.vic_regs[0x7A] & 0x80)) s.next_raster_interrupt *= 2;
    s.raster_interrupt_enabled = (s.vic_regs[0x1a] & 1) != 0;

    s.y_scale = s.vic_regs[0x5B];
    s.h640 = (s.vic_regs[0x31] & 0x80) != 0;
    s.v400 = (s.vic_regs[0x31] & 0x08) != 0;
    s.viciii_attribs = (s.vic_regs[0x31] & 0x20) != 0;
    s.chargen_x = (s.vic_regs[0x4c] | (s.vic_regs[0x4d] << 8)) & 0xfff;
    s.chargen_x -= SCREEN_POSITION;
    s.chargen_y = (s.vic_regs[0x4e] | (s.vic_regs[0x4f] << 8)) & 0xfff;

    s.top_border_y = (s.vic_regs[0x48] | (s.vic_regs[0x49] << 8)) & 0xfff;
    s.bottom_border_y = (s.vic_regs[0x4A] | (s.vic_regs[0x4B] << 8)) & 0xfff;
    s.side_border_width = (s.vic_regs[0x5C] | (s.vic_regs[0x5D] << 8)) & 0xfff;
    s.left_border = s.side_border_width - SCREEN_POSITION;
    s.right_border = 800 - s.side_border_width - SCREEN_POSITION;
    s.x_scale_120 = s.vic_regs[0x5A];
    s.x_step = s.x_scale_120 / 120.0f;
    if (!s.h640) s.x_step /= 2;

    if (s.sixteenbit_mode && !(s.vic_regs[0x54] & 4))
        s.charset_size = 8192 * 8;

    if (s.screen_size > MAX_SCREEN_SIZE) {
        std::println(stderr, "etherdbg: implausible screen size {} bytes", s.screen_size);
        return false;
    }

    if (verbose)
        std::println("Screen: {}x{} at ${:07X}, charset at ${:07X}",
                     s.screen_width, s.screen_rows,
                     s.screen_address, s.charset_address);

    /* Fetch screen RAM */
    if (verbose) std::println("Fetching screen data ({} bytes)...", s.screen_size);
    s.screen_data = cmd_read_memory(transport, s.screen_address, s.screen_size, false);
    if (s.screen_data.size() < s.screen_size) {
        std::println(stderr, "etherdbg: failed to read screen data");
        return false;
    }

    /* Fetch colour RAM */
    if (verbose) std::println("Fetching colour data...");
    s.colour_data = cmd_read_memory(transport, 0xff80000 + s.colour_address,
                                     s.screen_size, false);
    if (s.colour_data.size() < s.screen_size) {
        std::println(stderr, "etherdbg: failed to read colour data");
        return false;
    }

    /* Fetch charset */
    if (verbose) std::println("Fetching charset ({} bytes)...", s.charset_size);
    s.char_data = cmd_read_memory(transport, s.charset_address, s.charset_size, false);
    if (s.char_data.size() < s.charset_size) {
        std::println(stderr, "etherdbg: failed to read charset");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// do_screen_shot_ascii - ANSI terminal output
// ---------------------------------------------------------------------------

int do_screen_shot_ascii(const VideoState& s)
{
    /* Top border */
    std::printf("\033[48;2;%d;%d;%dm", mega65_rgb(s, s.border_colour, 0, false),
                mega65_rgb(s, s.border_colour, 1, false),
                mega65_rgb(s, s.border_colour, 2, false));
    for (unsigned x = 0; x < s.screen_width + 2; x++)
        std::printf(" ");
    std::printf("\033[0m\n");

    for (unsigned y = 0; y < s.screen_rows; y++) {
        /* Left border */
        std::printf("\033[48;2;%d;%d;%dm ", mega65_rgb(s, s.border_colour, 0, false),
                    mega65_rgb(s, s.border_colour, 1, false),
                    mega65_rgb(s, s.border_colour, 2, false));

        for (unsigned x = 0; x < s.screen_width; x++) {
            int char_value = s.screen_data[y * s.screen_line_step + x * (1 + s.sixteenbit_mode)];
            if (s.sixteenbit_mode)
                char_value |= (s.screen_data[y * s.screen_line_step + x * (1 + s.sixteenbit_mode) + 1] << 8);

            int colour_value = s.colour_data[y * s.screen_line_step + x * (1 + s.sixteenbit_mode)];
            if (s.sixteenbit_mode)
                colour_value |= (s.colour_data[y * s.screen_line_step + x * (1 + s.sixteenbit_mode) + 1] << 8);

            int char_id;
            int char_bg;
            if (s.extended_background_mode) {
                char_id = char_value & 0x3f;
                char_bg = s.vic_regs[0x21 + ((char_value >> 6) & 3)];
            }
            else {
                char_id = char_value & 0x1fff;
                char_bg = s.background_colour;
            }

            int fg_colour = colour_value & 0xff;
            bool glyph_full_colour = false;
            bool glyph_bold = false;
            bool glyph_reverse = false;

            if (s.viciii_attribs && !s.multicolour_mode) {
                glyph_reverse = (colour_value & 0x0020) != 0;
                glyph_bold = (colour_value & 0x0040) != 0;
                if (glyph_bold && !glyph_reverse)
                    fg_colour |= 0x10;
            }
            bool glyph_altpal = glyph_bold && glyph_reverse;

            if ((s.vic_regs[0x54] & 2) && char_id < 0x100) glyph_full_colour = true;
            if ((s.vic_regs[0x54] & 4) && char_id > 0xFF) glyph_full_colour = true;
            if (colour_value & 0x0800) glyph_full_colour = true;

            int fg = fg_colour;
            int bg = char_bg;
            if (glyph_reverse && !glyph_bold) {
                bg = fg_colour;
                fg = char_bg;
            }

            std::printf("\033[48;2;%d;%d;%dm\033[38;2;%d;%d;%dm",
                        mega65_rgb(s, bg, 0, glyph_altpal),
                        mega65_rgb(s, bg, 1, glyph_altpal),
                        mega65_rgb(s, bg, 2, glyph_altpal),
                        mega65_rgb(s, fg, 0, glyph_altpal),
                        mega65_rgb(s, fg, 1, glyph_altpal),
                        mega65_rgb(s, fg, 2, glyph_altpal));

            if (glyph_full_colour)
                std::printf("?");
            else
                print_screencode(char_id & 0xff, s.upper_case);
        }

        /* Right border + reset */
        std::printf("\033[48;2;%d;%d;%dm ", mega65_rgb(s, s.border_colour, 0, false),
                    mega65_rgb(s, s.border_colour, 1, false),
                    mega65_rgb(s, s.border_colour, 2, false));
        std::printf("\033[0m\n");
    }

    /* Bottom border */
    std::printf("\033[48;2;%d;%d;%dm", mega65_rgb(s, s.border_colour, 0, false),
                mega65_rgb(s, s.border_colour, 1, false),
                mega65_rgb(s, s.border_colour, 2, false));
    for (unsigned x = 0; x < s.screen_width + 2; x++)
        std::printf(" ");
    std::printf("\033[0m\n");

    return 0;
}

// ---------------------------------------------------------------------------
// do_screen_shot_png - PNG pixel-exact rendering
// ---------------------------------------------------------------------------

#ifdef HAS_LIBPNG

static int set_pixel(png_bytep* rows, int x, int y, int max_y,
                     int r, int g, int b)
{
    if (y < 0 || y > max_y || x < 0 || x > 719)
        return 1;
    rows[y][x * 3 + 0] = r;
    rows[y][x * 3 + 1] = g;
    rows[y][x * 3 + 2] = b;
    return 0;
}

static void paint_screen(const VideoState& s, [[maybe_unused]] Transport& transport,
                         png_bytep* rows, int min_y, int max_y_limit)
{
    int y_position = s.chargen_y;
    int max_height = s.is_pal_mode ? 576 : 480;

    for (unsigned cy = 0; cy < s.screen_rows; cy++) {
        if (y_position >= max_height) break;
        int x_position = s.chargen_x;
        int xc = 0;
        bool is_foreground = false;
        bool transparent_background = false;

        for (unsigned cx = 0; cx < s.screen_width; cx++) {
            int char_value = s.screen_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode)];
            if (s.sixteenbit_mode)
                char_value |= (s.screen_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode) + 1] << 8);

            int colour_value = s.colour_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode)];
            if (s.sixteenbit_mode) {
                colour_value = colour_value << 8;
                colour_value |= s.colour_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode) + 1];
            }

            int bg_colour = s.background_colour;
            int char_id;
            if (s.extended_background_mode) {
                char_id = char_value & 0x3f;
                bg_colour = s.vic_regs[0x21 + ((char_value >> 6) & 3)];
            }
            else {
                char_id = char_value & 0x1fff;
            }
            int glyph_width_deduct = char_value >> 13;

            int foreground_colour = colour_value & 0x0f;
            bool glyph_flip_vertical = (colour_value & 0x8000) != 0;
            bool glyph_flip_horizontal = (colour_value & 0x4000) != 0;
            bool glyph_with_alpha = (colour_value & 0x2000) != 0;
            bool glyph_goto = (colour_value & 0x1000) != 0;
            bool glyph_full_colour = false;
            bool glyph_underline = false;
            bool glyph_bold = false;
            bool glyph_reverse = false;
            bool glyph_altpal = false;

            if (s.viciii_attribs && !s.multicolour_mode) {
                glyph_reverse = (colour_value & 0x0020) != 0;
                glyph_bold = (colour_value & 0x0040) != 0;
                glyph_underline = (colour_value & 0x0080) != 0;
                glyph_altpal = glyph_bold && glyph_reverse;
                if (glyph_bold && !glyph_reverse)
                    foreground_colour |= 0x10;
            }
            if (s.multicolour_mode)
                foreground_colour = colour_value & 0xff;

            uint8_t bitmap_multi_colour = 0;
            if (s.bitmap_mode) {
                char_value = s.screen_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode)];
                foreground_colour = char_value & 0xf;
                bg_colour = char_value >> 4;
                bitmap_multi_colour = s.colour_data[cy * s.screen_line_step + cx * (1 + s.sixteenbit_mode)];
            }

            if ((s.vic_regs[0x54] & 2) && char_id < 0x100) glyph_full_colour = true;
            if ((s.vic_regs[0x54] & 4) && char_id > 0xFF) glyph_full_colour = true;
            bool glyph_4bit = (colour_value & 0x0800) != 0;
            if (colour_value & 0x0400) glyph_width_deduct += 8;

            int glyph_width = glyph_4bit ? 16 : 8;
            glyph_width -= glyph_width_deduct;

            for (int yy = 0; yy < 8; yy++) {
                int glyph_row = glyph_flip_vertical ? (7 - yy) : yy;
                uint8_t glyph_data[8] = {};

                if (glyph_full_colour) {
                    /* For full-colour, would need to fetch from char_id * 64 */
                    /* Use cached char_data if available, else zeros */
                    size_t offset = char_id * 64 + glyph_row * 8;
                    if (offset + 8 <= s.char_data.size())
                        std::memcpy(glyph_data, &s.char_data[offset], 8);
                }
                else if (!s.bitmap_mode) {
                    size_t ci = static_cast<size_t>(char_id) * 8 + glyph_row;
                    uint8_t bits = (ci < s.char_data.size()) ? s.char_data[ci] : 0;
                    for (int i = 0; i < 8; i++)
                        glyph_data[i] = (bits >> i) & 1 ? 0xff : 0;
                }
                else {
                    uint32_t addr = s.charset_address & (s.h640 ? 0xfc000u : 0xfe000u);
                    addr += cx * 8 + cy * (s.h640 ? 640u : 320u) + glyph_row;
                    /* Read single byte from bitmap - use cached data if possible */
                    uint8_t pixels = 0;
                    if (addr >= s.charset_address) {
                        size_t off = addr - s.charset_address;
                        if (off < s.char_data.size())
                            pixels = s.char_data[off];
                    }
                    for (int i = 0; i < 8; i++)
                        glyph_data[i] = (pixels >> i) & 1 ? 0xff : 0;
                }

                if (glyph_flip_horizontal) {
                    uint8_t tmp[8];
                    std::memcpy(tmp, glyph_data, 8);
                    for (int i = 0; i < 8; i++) glyph_data[i] = tmp[7 - i];
                }
                if (glyph_reverse && !glyph_bold)
                    for (int i = 0; i < 8; i++) glyph_data[i] = 0xff - glyph_data[i];
                if (glyph_underline && yy == 7)
                    for (int i = 0; i < 8; i++) glyph_data[i] = 0xff;

                xc = 0;
                if (glyph_goto) {
                    x_position = s.chargen_x + (char_value & 0x3ff);
                    transparent_background = (colour_value & 0x8000) != 0;
                }
                else {
                    for (float xx = 0; xx < glyph_width; xx += s.x_step) {
                        int r = mega65_rgb(s, bg_colour, 0, glyph_altpal);
                        int g = mega65_rgb(s, bg_colour, 1, glyph_altpal);
                        int b = mega65_rgb(s, bg_colour, 2, glyph_altpal);
                        is_foreground = false;

                        if (glyph_4bit) {
                            int c = glyph_data[((int)xx) / 2];
                            c = ((int)xx & 1) ? (c >> 4) : (c & 0xf);
                            if (glyph_with_alpha) {
                                r = (mega65_rgb(s, foreground_colour, 0, glyph_altpal) * c + r * (15 - c)) / 15;
                                g = (mega65_rgb(s, foreground_colour, 1, glyph_altpal) * c + g * (15 - c)) / 15;
                                b = (mega65_rgb(s, foreground_colour, 2, glyph_altpal) * c + b * (15 - c)) / 15;
                            }
                            else {
                                if (c == 0) { /* bg */ }
                                else if (c == 0xf) {
                                    r = mega65_rgb(s, foreground_colour, 0, glyph_altpal);
                                    g = mega65_rgb(s, foreground_colour, 1, glyph_altpal);
                                    b = mega65_rgb(s, foreground_colour, 2, glyph_altpal);
                                }
                                else {
                                    r = mega65_rgb(s, c, 0, glyph_altpal);
                                    g = mega65_rgb(s, c, 1, glyph_altpal);
                                    b = mega65_rgb(s, c, 2, glyph_altpal);
                                }
                            }
                            if (c) is_foreground = true;
                        }
                        else if (glyph_full_colour) {
                            if (glyph_with_alpha) {
                                int a = glyph_data[(int)xx];
                                r = (mega65_rgb(s, foreground_colour, 0, glyph_altpal) * a + r * (255 - a)) >> 8;
                                g = (mega65_rgb(s, foreground_colour, 1, glyph_altpal) * a + g * (255 - a)) >> 8;
                                b = (mega65_rgb(s, foreground_colour, 2, glyph_altpal) * a + b * (255 - a)) >> 8;
                                if (foreground_colour) is_foreground = true;
                            }
                            else {
                                r = mega65_rgb(s, glyph_data[(int)xx], 0, glyph_altpal);
                                g = mega65_rgb(s, glyph_data[(int)xx], 1, glyph_altpal);
                                b = mega65_rgb(s, glyph_data[(int)xx], 2, glyph_altpal);
                            }
                        }
                        else if (s.multicolour_mode && ((foreground_colour & 8) || s.bitmap_mode)) {
                            int bits = 0;
                            if (glyph_data[6 - (((int)xx) & 0x6)]) bits |= 1;
                            if (glyph_data[7 - (((int)xx) & 0x6)]) bits |= 2;
                            int colour = 0;
                            if (!s.bitmap_mode) {
                                switch (bits) {
                                    case 0: colour = s.vic_regs[0x21]; break;
                                    case 1: is_foreground = true; colour = s.vic_regs[0x22]; break;
                                    case 2: is_foreground = true; colour = s.vic_regs[0x23]; break;
                                    case 3: is_foreground = true; colour = foreground_colour & 7; break;
                                }
                            }
                            else {
                                switch (bits) {
                                    case 0: is_foreground = true; colour = s.vic_regs[0x21]; break;
                                    case 1: colour = bg_colour; break;
                                    case 2: is_foreground = true; colour = foreground_colour; break;
                                    case 3: is_foreground = true; colour = bitmap_multi_colour & 0xf; break;
                                }
                            }
                            r = mega65_rgb(s, colour, 0, glyph_altpal);
                            g = mega65_rgb(s, colour, 1, glyph_altpal);
                            b = mega65_rgb(s, colour, 2, glyph_altpal);
                        }
                        else {
                            if (glyph_data[7 - (int)xx]) {
                                r = mega65_rgb(s, foreground_colour, 0, glyph_altpal);
                                g = mega65_rgb(s, foreground_colour, 1, glyph_altpal);
                                b = mega65_rgb(s, foreground_colour, 2, glyph_altpal);
                                is_foreground = true;
                            }
                        }

                        for (unsigned yc = 0; yc <= s.y_scale; yc++) {
                            int py = y_position + yc + yy * (1 + s.y_scale);
                            int px = x_position + xc;
                            if (py >= min_y && py <= max_y_limit
                                && py < static_cast<int>(s.bottom_border_y)
                                && py >= static_cast<int>(s.top_border_y)
                                && px < static_cast<int>(s.right_border)
                                && px >= static_cast<int>(s.left_border)) {
                                if (is_foreground || !transparent_background)
                                    set_pixel(rows, px, py, max_height - 1, r, g, b);
                            }
                        }
                        xc++;
                    }
                }
            }
            x_position += xc;
        }
        y_position += 8 * (1 + s.y_scale);
    }
}

#endif // HAS_LIBPNG

int do_screen_shot_png([[maybe_unused]] Transport& transport,
                       [[maybe_unused]] const VideoState& s,
                       [[maybe_unused]] std::string_view filename)
{
#ifdef HAS_LIBPNG
    int max_height = s.is_pal_mode ? 576 : 480;

    FILE* f = std::fopen(std::string(filename).c_str(), "wb");
    if (!f) {
        std::println(stderr, "etherdbg: cannot open '{}' for writing", filename);
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    if (!png) { std::fclose(f); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); std::fclose(f); return -1; }

    png_init_io(png, f);
    png_set_IHDR(png, info, 720, max_height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);

    /* Allocate rows, fill with border colour */
    std::vector<std::vector<uint8_t>> row_bufs(max_height, std::vector<uint8_t>(720 * 3));
    std::vector<png_bytep> rows(max_height);
    for (int y = 0; y < max_height; y++) {
        rows[y] = row_bufs[y].data();
        for (int x = 0; x < 720; x++) {
            rows[y][x * 3 + 0] = mega65_rgb(s, s.border_colour, 0, false);
            rows[y][x * 3 + 1] = mega65_rgb(s, s.border_colour, 1, false);
            rows[y][x * 3 + 2] = mega65_rgb(s, s.border_colour, 2, false);
        }
    }

    /* Fill non-border area with background */
    for (unsigned y = s.top_border_y; y < s.bottom_border_y && y < static_cast<unsigned>(max_height); y++) {
        for (unsigned x = s.left_border; x < s.right_border; x++) {
            rows[y][x * 3 + 0] = mega65_rgb(s, s.background_colour, 0, false);
            rows[y][x * 3 + 1] = mega65_rgb(s, s.background_colour, 1, false);
            rows[y][x * 3 + 2] = mega65_rgb(s, s.background_colour, 2, false);
        }
    }

    /* Render screen content */
    paint_screen(s, transport, rows.data(), 0, max_height);

    /* Write PNG */
    for (int y = 0; y < max_height; y++)
        png_write_row(png, rows[y]);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);

    std::println("Wrote screenshot to {}", filename);
    return 0;
#else
    std::println(stderr, "etherdbg: PNG support not compiled (install libpng-dev and rebuild with -DHAS_LIBPNG)");
    return 1;
#endif
}

// ---------------------------------------------------------------------------
// cmd_screen_shot - high-level command
// ---------------------------------------------------------------------------

int cmd_screen_shot(Transport& transport, std::string_view filename,
                    bool verbose)
{
    VideoState state;
    if (!get_video_state(transport, state, verbose))
        return -1;

    do_screen_shot_ascii(state);

    if (filename == "0") {
        if (verbose)
            std::println("No PNG screenshot requested.");
        return 0;
    }

    std::string png_filename;
    if (filename.empty()) {
        /* Auto-generate filename */
        for (int n = 0; n < 1000000; n++) {
            png_filename = std::format("mega65-screen-{:06d}.png", n);
            if (!std::filesystem::exists(png_filename))
                break;
        }
    }
    else {
        png_filename = std::string(filename);
    }

    return do_screen_shot_png(transport, state, png_filename);
}

} // namespace etherdbg
