/*
 * screen.h - MEGA65 screen capture via Ethernet debug
 *
 * Ported from mega65-tools/src/tools/screen_shot.c
 * Reads VIC-IV state and renders screen content.
 */

#pragma once

#include "transport.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace etherdbg {

struct VideoState {
    /* VIC-IV register block ($FFD3000-$FFD36FF) */
    std::vector<uint8_t> vic_regs;   /* 0x700 bytes: regs + 2 palettes */

    /* Derived from registers */
    uint32_t screen_address = 0;
    uint32_t charset_address = 0;
    uint32_t colour_address = 0;
    unsigned screen_line_step = 0;
    unsigned screen_width = 0;
    unsigned screen_rows = 0;
    unsigned screen_size = 0;
    unsigned charset_size = 0;
    bool upper_case = true;
    bool sixteenbit_mode = false;
    bool extended_background_mode = false;
    bool multicolour_mode = false;
    bool bitmap_mode = false;
    bool is_pal_mode = false;
    bool h640 = false;
    bool v400 = false;
    bool viciii_attribs = false;

    int border_colour = 0;
    int background_colour = 0;

    unsigned y_scale = 0;
    unsigned chargen_x = 0;
    unsigned chargen_y = 0;
    unsigned top_border_y = 0;
    unsigned bottom_border_y = 0;
    unsigned side_border_width = 0;
    unsigned left_border = 0;
    unsigned right_border = 0;
    unsigned x_scale_120 = 0;
    float x_step = 0;

    unsigned current_physical_raster = 0;
    unsigned next_raster_interrupt = 0;
    bool raster_interrupt_enabled = false;

    /* Fetched data */
    std::vector<uint8_t> screen_data;
    std::vector<uint8_t> colour_data;
    std::vector<uint8_t> char_data;
};

/*
 * Fetch the current video state from the MEGA65.
 * Reads VIC-IV registers, screen RAM, colour RAM, and charset.
 * Returns true on success.
 */
bool get_video_state(Transport& transport, VideoState& state, bool verbose);

/*
 * Render an ASCII screenshot to stdout using ANSI escape codes.
 * Displays screen content with approximate colours in the terminal.
 */
int do_screen_shot_ascii(const VideoState& state);

/*
 * Render a PNG screenshot to a file.
 * Returns 0 on success, -1 on error, 1 if PNG support not compiled in.
 */
int do_screen_shot_png(Transport& transport, const VideoState& state,
                       std::string_view filename);

/*
 * High-level screen shot command.
 * Fetches video state, prints ASCII to terminal, and optionally saves PNG.
 * If filename is empty, auto-generates "mega65-screen-NNNNNN.png".
 * If filename is "0", skips PNG output.
 */
int cmd_screen_shot(Transport& transport, std::string_view filename,
                    bool verbose);

} // namespace etherdbg
