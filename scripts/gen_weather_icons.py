# -*- coding: utf-8 -*-
"""
生成 LVGL v9 天气图标 (A8 alpha 位图), 实心黑色风格, 贴合 234.png.
每个图标输出大(当前天气)/小(7天预报)两种尺寸.
用 4x 超采样 + LANCZOS 缩小得到平滑边缘 (A8 保留灰度抗锯齿, 上屏时按阈值二值化).

输出: firmware/components/ui_app/fonts/weather_icons.c
用法: python gen_weather_icons.py
"""
import os
from PIL import Image, ImageDraw

SS = 4  # 超采样倍数

# 目标尺寸
BIG_W, BIG_H = 78, 60
SM_W,  SM_H  = 42, 34

OUT = os.path.join(os.path.dirname(__file__),
                   "../firmware/components/ui_app/fonts/weather_icons.c")


def new_canvas(w, h):
    img = Image.new("L", (w * SS, h * SS), 0)  # 0=透明
    return img, ImageDraw.Draw(img)


def fill_ellipse(d, cx, cy, r, col=255):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)


def draw_cloud(d, w, h, cy_ratio=0.52, scale=1.0):
    """实心云朵: 三个圆 + 底部矩形拼成经典云形. 返回云底 y (用于附加雨雪)."""
    W, H = w * SS, h * SS
    cx = W * 0.5
    cy = H * cy_ratio
    base_r = H * 0.20 * scale
    # 大圆(中) + 左右小圆 + 顶圆
    fill_ellipse(d, cx, cy, base_r * 1.15)
    fill_ellipse(d, cx - base_r * 1.15, cy + base_r * 0.35, base_r * 0.85)
    fill_ellipse(d, cx + base_r * 1.15, cy + base_r * 0.35, base_r * 0.85)
    fill_ellipse(d, cx - base_r * 0.35, cy - base_r * 0.55, base_r * 0.80)
    fill_ellipse(d, cx + base_r * 0.45, cy - base_r * 0.35, base_r * 0.70)
    # 底部矩形拉平
    bottom = cy + base_r * 0.35 + base_r * 0.85
    d.rectangle([cx - base_r * 1.15, cy, cx + base_r * 1.15, bottom], fill=255)
    return bottom


def icon_cloud(w, h):
    img, d = new_canvas(w, h)
    draw_cloud(d, w, h, cy_ratio=0.45)
    return img


def icon_fog(w, h):
    img, d = new_canvas(w, h)
    bottom = draw_cloud(d, w, h, cy_ratio=0.36, scale=0.92)
    W, H = w * SS, h * SS
    lw = int(H * 0.09)
    y1 = bottom + H * 0.10
    y2 = y1 + H * 0.16
    d.line([W * 0.20, y1, W * 0.80, y1], fill=255, width=lw)
    d.line([W * 0.26, y2, W * 0.74, y2], fill=255, width=lw)
    return img


def _rain(img, d, w, h, bottom, n, length):
    W, H = w * SS, h * SS
    lw = int(H * 0.075)
    step = (W * 0.5) / n
    x0 = W * 0.5 - step * (n - 1) / 2
    for i in range(n):
        x = x0 + i * step
        d.line([x, bottom + H * 0.06, x - W * 0.05, bottom + H * 0.06 + length],
               fill=255, width=lw)


def icon_rain(w, h):
    img, d = new_canvas(w, h)
    bottom = draw_cloud(d, w, h, cy_ratio=0.34, scale=0.92)
    _rain(img, d, w, h, bottom, 3, (h * SS) * 0.20)
    return img


def icon_heavyrain(w, h):
    img, d = new_canvas(w, h)
    bottom = draw_cloud(d, w, h, cy_ratio=0.32, scale=0.90)
    _rain(img, d, w, h, bottom, 4, (h * SS) * 0.24)
    return img


def icon_snow(w, h):
    img, d = new_canvas(w, h)
    bottom = draw_cloud(d, w, h, cy_ratio=0.34, scale=0.92)
    W, H = w * SS, h * SS
    r = H * 0.045
    for i, fx in enumerate([0.32, 0.5, 0.68]):
        y = bottom + H * (0.12 if i % 2 == 0 else 0.20)
        fill_ellipse(d, W * fx, y, r)
    return img


def icon_sun(w, h):
    img, d = new_canvas(w, h)
    W, H = w * SS, h * SS
    cx, cy = W * 0.5, H * 0.5
    r = H * 0.22
    fill_ellipse(d, cx, cy, r)
    lw = int(H * 0.06)
    import math
    for k in range(8):
        a = math.pi * 2 * k / 8
        x1 = cx + math.cos(a) * r * 1.5
        y1 = cy + math.sin(a) * r * 1.5
        x2 = cx + math.cos(a) * r * 2.0
        y2 = cy + math.sin(a) * r * 2.0
        d.line([x1, y1, x2, y2], fill=255, width=lw)
    return img


def icon_partly(w, h):
    img, d = new_canvas(w, h)
    W, H = w * SS, h * SS
    # 太阳在左上
    cx, cy = W * 0.34, H * 0.34
    r = H * 0.16
    fill_ellipse(d, cx, cy, r)
    lw = int(H * 0.05)
    import math
    for k in range(8):
        a = math.pi * 2 * k / 8
        d.line([cx + math.cos(a) * r * 1.4, cy + math.sin(a) * r * 1.4,
                cx + math.cos(a) * r * 1.9, cy + math.sin(a) * r * 1.9],
               fill=255, width=lw)
    # 云盖在右下(先清除云占位区避免光芒穿透太多, 再画云)
    draw_cloud(d, w, h, cy_ratio=0.58, scale=0.82)
    return img


ICONS = {
    "sun": icon_sun,
    "partly": icon_partly,
    "cloud": icon_cloud,
    "fog": icon_fog,
    "rain": icon_rain,
    "heavyrain": icon_heavyrain,
    "snow": icon_snow,
}


def render(fn, w, h):
    img = fn(w, h)
    img = img.resize((w, h), Image.LANCZOS)
    return img


def emit_img(f, name, img):
    w, h = img.size
    data = img.tobytes()  # L 模式, 每像素 1 字节 = A8
    f.write(f"static const uint8_t {name}_map[] = {{\n")
    for i, b in enumerate(data):
        if i % 16 == 0:
            f.write("    ")
        f.write(f"0x{b:02x},")
        f.write("\n" if (i + 1) % 16 == 0 else " ")
    if len(data) % 16 != 0:
        f.write("\n")
    f.write("};\n")
    f.write(f"const lv_image_dsc_t {name} = {{\n")
    f.write("    .header = { .magic = LV_IMAGE_HEADER_MAGIC, "
            f".cf = LV_COLOR_FORMAT_A8, .flags = 0, .w = {w}, .h = {h}, .stride = {w} }},\n")
    f.write(f"    .data_size = {len(data)},\n")
    f.write(f"    .data = {name}_map,\n")
    f.write("};\n\n")


def main():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated weather icons (A8) — gen_weather_icons.py */\n")
        f.write("#include \"lvgl.h\"\n\n")
        for key, fn in ICONS.items():
            emit_img(f, f"wi_{key}_big", render(fn, BIG_W, BIG_H))
            emit_img(f, f"wi_{key}_sm", render(fn, SM_W, SM_H))
    print(f"written: {OUT}")


if __name__ == "__main__":
    main()
