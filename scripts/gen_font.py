"""Generate a custom LVGL bitmap font for missing Chinese characters.
Uses Windows YaHei font (msyh.ttc) to render characters as 1-bit bitmaps."""

import struct
import os
import math
from PIL import Image, ImageDraw, ImageFont

# Characters we need (that are missing from the built-in CJK font)
MISSING_CHARS = "积银总资产涨跌净额Au白银"

# Font path
FONT_PATH = "C:/Windows/Fonts/msyh.ttc"

def generate_font(output_c, font_size=16, bpp=1):
    """Generate a custom LVGL font C file with specific characters."""

    # Load font
    font = ImageFont.truetype(FONT_PATH, font_size)

    chars = MISSING_CHARS
    glyphs = []

    # Render each character
    for ch in chars:
        cp = ord(ch)
        # Use PIL to get the glyph mask
        mask = font.getmask(ch, mode='L')
        bbox = font.getbbox(ch)

        if bbox is None:
            print(f"Warning: {ch} (U+{cp:04X}) has no glyph")
            continue

        x0, y0, x1, y1 = bbox
        w = x1 - x0
        h = y1 - y0

        if w <= 0 or h <= 0:
            print(f"Warning: {ch} (U+{cp:04X}) has empty bbox ({w}x{h})")
            continue

        # Create image and render
        img = Image.new('L', (w, h), 255)
        draw = ImageDraw.Draw(img)
        draw.font = font
        # PIL 10+ way to draw with mask
        img_draw = ImageDraw.Draw(img)
        img_draw.text((-x0, -y0), ch, fill=0, font=font)

        # Convert to 1-bit (threshold at 128)
        # LVGL standard: 0 = transparent, 1 = black
        bits = []
        for y in range(h):
            row = 0
            for x in range(w):
                px = img.getpixel((x, y))
                # Invert: LVGL uses 0 for transparent, 1 for drawn
                bit = 1 if px < 128 else 0
                row = (row << 1) | bit
                if (x % 8) == 7 or x == w - 1:
                    # Need to pad remaining bits with 0
                    remaining = 7 - (x % 8)
                    row <<= remaining
                    bits.append(row)
                    row = 0

        glyphs.append({
            'cp': cp,
            'char': ch,
            'w': w,
            'h': h,
            'adv_w': w,
            'ofs_x': 0,
            'ofs_y': 0,
            'data': bytes(bits)
        })

        print(f"  {ch} (U+{cp:04X}): {w}x{h}, {len(bits)} bytes")

    # Generate C file
    with open(output_c, 'w') as f:
        f.write('/* Auto-generated font for missing Chinese characters */\n')
        f.write('#include "lvgl.h"\n\n')

        # Bitmap data array
        bitmap_offset = 0
        all_bitmaps = b''

        # First pass: collect all bitmaps
        for g in glyphs:
            # Align to 4 bytes
            data = g['data']
            # Pad to multiple of 4
            while len(data) % 4 != 0:
                data += b'\x00'
            g['bitmap_offset'] = bitmap_offset
            bitmap_offset += len(data)
            all_bitmaps += data

        # Write bitmap array
        f.write(f'static const uint8_t custom_font_bitmap[] = {{\n')
        for i, b in enumerate(all_bitmaps):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'0x{b:02x}, ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        if len(all_bitmaps) % 16 != 0:
            f.write('\n')
        f.write('};\n\n')

        # Glyph descriptor array
        f.write('static const lv_font_glyph_dsc_t custom_font_glyph_dsc[] = {\n')
        for g in glyphs:
            f.write(f'    {{ .bitmap_index = {g["bitmap_offset"]}, .adv_w = {g["adv_w"]}, '
                    f'.box_w = {g["w"]}, .box_h = {g["h"]}, '
                    f'.ofs_x = {g["ofs_x"]}, .ofs_y = {g["ofs_y"]} }}, '
                    f'/* U+{g["cp"]:04X} {g["char"]} */\n')
        f.write('};\n\n')

        # Glyph map (unicode -> index mapping)
        f.write('/* Unicode to glyph index mapping */\n')
        f.write('static const uint16_t custom_font_unicode_list[] = {\n    ')
        for i, g in enumerate(glyphs):
            f.write(f'0x{g["cp"]:04X}, ')
            if (i + 1) % 10 == 0:
                f.write('\n    ')
        f.write('\n};\n\n')

        # The font descriptor
        f.write('/* Custom font descriptor */\n')
        # Need to get font metrics from the actual font
        ascent = font.getmetrics()[0]
        f.write(f'const lv_font_t custom_font = {{\n')
        f.write(f'    .get_glyph_dsc = NULL,  /* will use lv_font_get_glyph_dsc */\n')
        f.write(f'    .get_glyph_bitmap = NULL,  /* will use lv_font_get_glyph_bitmap */\n')
        f.write(f'    .line_height = {font_size + 2},\n')
        f.write(f'    .base_line = 0,\n')
        f.write(f'    .subpx = LV_FONT_SUBPX_NONE,\n')
        f.write(f'    .underline_position = -2,\n')
        f.write(f'    .underline_thickness = 1,\n')
        f.write(f'    .dsc = NULL,\n')
        f.write(f'    .fallback = NULL,\n')
        f.write(f'}};\n\n')

        # The glyph dsc and unicode list need to be connected to the font
        # For LVGL v9 we need to use lv_font_set_glyph_dsc_cb

    print(f"\nGenerated {output_c} with {len(glyphs)} glyphs")

if __name__ == '__main__':
    generate_font('custom_chinese_font.c', font_size=16)
