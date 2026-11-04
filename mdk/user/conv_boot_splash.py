# -*- coding: utf-8 -*-
"""
Convert 潘.jpg to RGB565 raw C array for ST7735 128x128 LCD boot splash.
Image is 96x96 centered on screen (18KB Flash) to fit STM32F103C8 64KB budget.
"""
from PIL import Image

SRC = r"d:/RTOS/项目/潘.jpg"
OUT = r"d:/RTOS/项目/Freertos的嵌入式智能天气时钟/app/boot_splash.c"
W, H = 96, 96   # centered on 128x128 screen, leaves 16px border each side

def to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def main():
    im = Image.open(SRC).convert("RGB")
    im = im.resize((W, H), Image.LANCZOS)
    px = im.load()

    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = to_rgb565(r, g, b)
            vals.append((v >> 8) & 0xFF)
            vals.append(v & 0xFF)

    data_size = len(vals)
    print(f"Image: {W}x{H}, RGB565 raw = {data_size} bytes ({data_size/1024:.1f} KB)")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated boot splash image (raw RGB565) */\n")
        f.write(f"/* Source: Pan.jpg resized to {W}x{H}, RGB565, {data_size} bytes */\n")
        f.write("#include <stdint.h>\n")
        f.write('#include "stimage.h"\n\n')
        f.write(f"const uint16_t boot_splash_width  = {W};\n")
        f.write(f"const uint16_t boot_splash_height = {H};\n")
        f.write(f"/* {W}x{H} RGB565 pixel data, {data_size} bytes */\n")
        f.write(f"const uint8_t boot_splash_data[{data_size}] = {{\n")
        line = "    "
        for i, b in enumerate(vals):
            line += f"0x{b:02X},"
            if (i + 1) % 16 == 0:
                f.write(line + "\n")
                line = "    "
        if line.strip():
            f.write(line.rstrip(",") + "\n")
        f.write("};\n")

    print(f"Written: {OUT}")

if __name__ == "__main__":
    main()
