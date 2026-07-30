# -*- coding: utf-8 -*-
"""
生成 LVGL v9 天气图标 (A8 alpha 位图), 线条描边(空心轮廓)风格, 贴合 HTML 预览.
每个图标输出大(当前天气)/小(7天预报)两种尺寸.

做法: 4x 超采样绘制实心 mask, 用形态学腐蚀相减得到"均匀宽度的外轮廓线"
(云这种多圆重叠也能得到干净单一轮廓), 太阳光芒/雨/雪/雾等细节本就是线条直接保留;
最后 LANCZOS 缩小得平滑抗锯齿边 (A8 保留灰度, 上屏按阈值二值化).

输出: firmware/components/ui_app/fonts/weather_icons.c
用法: python gen_weather_icons.py
"""
import os
import math
from PIL import Image, ImageDraw, ImageChops, ImageFilter

SS = 4  # 超采样倍数

# 目标尺寸
BIG_W, BIG_H = 78, 60
SM_W,  SM_H  = 42, 34

# 线宽 (超采样域像素). 最终线宽 ≈ STROKE / SS. 取 8 → 约 2px, 反射屏清晰不糊.
STROKE = 8

OUT = os.path.join(os.path.dirname(__file__),
                   "../firmware/components/ui_app/fonts/weather_icons.c")


def new_canvas(w, h):
    img = Image.new("L", (w * SS, h * SS), 0)  # 0=透明
    return img, ImageDraw.Draw(img)


def fill_ellipse(d, cx, cy, r, col=255):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)


def outline_of(mask, stroke=STROKE):
    """实心 mask → 均匀宽度外轮廓线 (原图 - 腐蚀图)."""
    eroded = mask
    for _ in range(stroke):
        eroded = eroded.filter(ImageFilter.MinFilter(3))
    return ImageChops.subtract(mask, eroded)


def draw_cloud_mask(w, h, cy_ratio=0.52, scale=1.0):
    """实心云朵 mask: 三个圆 + 底部矩形拼成经典云形. 返回 (mask, 云底 y)."""
    img, d = new_canvas(w, h)
    W, H = w * SS, h * SS
    cx = W * 0.5
    cy = H * cy_ratio
    base_r = H * 0.20 * scale
    fill_ellipse(d, cx, cy, base_r * 1.15)
    fill_ellipse(d, cx - base_r * 1.15, cy + base_r * 0.35, base_r * 0.85)
    fill_ellipse(d, cx + base_r * 1.15, cy + base_r * 0.35, base_r * 0.85)
    fill_ellipse(d, cx - base_r * 0.35, cy - base_r * 0.55, base_r * 0.80)
    fill_ellipse(d, cx + base_r * 0.45, cy - base_r * 0.35, base_r * 0.70)
    bottom = cy + base_r * 0.35 + base_r * 0.85
    d.rectangle([cx - base_r * 1.15, cy, cx + base_r * 1.15, bottom], fill=255)
    return img, bottom


def icon_cloud(w, h):
    mask, _ = draw_cloud_mask(w, h, cy_ratio=0.45)
    return outline_of(mask)


def icon_fog(w, h):
    mask, bottom = draw_cloud_mask(w, h, cy_ratio=0.36, scale=0.92)
    img = outline_of(mask)
    d = ImageDraw.Draw(img)
    W, H = w * SS, h * SS
    lw = STROKE
    y1 = bottom + H * 0.10
    y2 = y1 + H * 0.16
    d.line([W * 0.20, y1, W * 0.80, y1], fill=255, width=lw)
    d.line([W * 0.26, y2, W * 0.74, y2], fill=255, width=lw)
    return img


def _rain(d, w, h, bottom, n, length):
    W, H = w * SS, h * SS
    lw = STROKE
    step = (W * 0.5) / n
    x0 = W * 0.5 - step * (n - 1) / 2
    for i in range(n):
        x = x0 + i * step
        d.line([x, bottom + H * 0.06, x - W * 0.05, bottom + H * 0.06 + length],
               fill=255, width=lw)


def icon_rain(w, h):
    mask, bottom = draw_cloud_mask(w, h, cy_ratio=0.34, scale=0.92)
    img = outline_of(mask)
    _rain(ImageDraw.Draw(img), w, h, bottom, 3, (h * SS) * 0.20)
    return img


def icon_heavyrain(w, h):
    mask, bottom = draw_cloud_mask(w, h, cy_ratio=0.32, scale=0.90)
    img = outline_of(mask)
    _rain(ImageDraw.Draw(img), w, h, bottom, 4, (h * SS) * 0.24)
    return img


def icon_snow(w, h):
    mask, bottom = draw_cloud_mask(w, h, cy_ratio=0.34, scale=0.92)
    img = outline_of(mask)
    d = ImageDraw.Draw(img)
    W, H = w * SS, h * SS
    r = H * 0.05
    for i, fx in enumerate([0.32, 0.5, 0.68]):
        y = bottom + H * (0.12 if i % 2 == 0 else 0.20)
        fill_ellipse(d, W * fx, y, r)   # 雪点保持实心小圆
    return img


def _sun_rays(d, cx, cy, r, lw, n=8, inner=1.35, outer=1.9):
    for k in range(n):
        a = math.pi * 2 * k / n
        d.line([cx + math.cos(a) * r * inner, cy + math.sin(a) * r * inner,
                cx + math.cos(a) * r * outer, cy + math.sin(a) * r * outer],
               fill=255, width=lw)


def icon_sun(w, h):
    img, d = new_canvas(w, h)
    W, H = w * SS, h * SS
    cx, cy = W * 0.5, H * 0.5
    r = H * 0.20
    lw = STROKE
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=lw)  # 空心圆
    _sun_rays(d, cx, cy, r, lw)
    return img


def icon_partly(w, h):
    img, d = new_canvas(w, h)
    W, H = w * SS, h * SS
    lw = STROKE
    # 太阳环在左上
    cx, cy = W * 0.34, H * 0.34
    r = H * 0.15
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=lw)
    _sun_rays(d, cx, cy, r, lw, inner=1.35, outer=1.85)
    # 云 (实心 mask) 盖右下: 先把云覆盖区的太阳线抹掉, 再叠云轮廓
    cloud_mask, _ = draw_cloud_mask(w, h, cy_ratio=0.60, scale=0.82)
    img = ImageChops.subtract(img, cloud_mask)          # 挖空被云遮住的太阳线
    img = ImageChops.lighter(img, outline_of(cloud_mask))  # 叠加云轮廓
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
    return img.resize((w, h), Image.LANCZOS)


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
        f.write("/* Auto-generated weather icons (A8, 线条描边) — gen_weather_icons.py */\n")
        f.write("#include \"lvgl.h\"\n\n")
        for key, fn in ICONS.items():
            emit_img(f, f"wi_{key}_big", render(fn, BIG_W, BIG_H))
            emit_img(f, f"wi_{key}_sm", render(fn, SM_W, SM_H))
    print(f"written: {OUT}")


if __name__ == "__main__":
    main()
