# -*- coding: utf-8 -*-
"""
从一张源图生成托盘/程序图标（多尺寸 .ico）。

用法:
    python gen_icons.py <源图.png>

输出（写到 tray_icons/）:
    connected.ico   原样（彩色），用于"已连接"
    idle.ico        灰度版，用于"未连接"
    app.ico         程序图标（任务栏 / 资源管理器），用彩色版

托盘图标走 Windows 的 SM_CXSMICON 尺寸（通常 16px），
所以小尺寸的可读性最重要 —— 缩完做一次锐化，避免糊成一团。
"""
import os
import sys

from PIL import Image, ImageEnhance, ImageFilter

OUT_DIR = "tray_icons"
SIZES = [16, 20, 24, 32, 48, 64, 128, 256]


def to_gray(img):
    """转成灰度，并稍微提一点对比度 —— 全灰会显得脏。"""
    g = img.convert("L").convert("RGBA")
    # 保留原图的 alpha
    g.putalpha(img.getchannel("A"))
    g = ImageEnhance.Contrast(g.convert("RGB")).enhance(1.15).convert("RGBA")
    g.putalpha(img.getchannel("A"))
    return g


def shrink_icon(src, size):
    """缩小到目标尺寸。小图锐化一下，不然托盘里看不清。"""
    out = src.resize((size, size), Image.LANCZOS)
    if size <= 32:
        out = out.filter(ImageFilter.UnsharpMask(radius=0.6, percent=80, threshold=2))
    return out


def save_ico(img, path):
    frames = [shrink_icon(img, s) for s in SIZES]
    frames[-1].save(path, format="ICO", sizes=[(s, s) for s in SIZES])
    print("  " + path)


def main():
    if len(sys.argv) < 2:
        print("用法: python gen_icons.py <源图.png>")
        return 1

    src_path = sys.argv[1]
    if not os.path.exists(src_path):
        print("找不到: " + src_path)
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)

    src = Image.open(src_path).convert("RGBA")

    # 裁掉四周透明留白，然后只保留 40% 的留白 —— 让图标在托盘里显得大一些。
    # （托盘图标只有 16px，边缘空白多的话主体会小得看不清。）
    bbox = src.getbbox()
    if bbox:
        src = src.crop(bbox)
        w, h = src.size
        side = max(w, h)
        # 四周留白 = 边长的 1.5%（原来是 11%，砍掉近九成）
        pad = int(side * 0.015)
        canvas = Image.new("RGBA", (side + pad * 2, side + pad * 2), (0, 0, 0, 0))
        canvas.paste(src, ((side - w) // 2 + pad, (side - h) // 2 + pad), src)
        src = canvas
        print("  （已裁剪留白: %dx%d -> %dx%d）" % (bbox[2]-bbox[0], bbox[3]-bbox[1], *src.size))

    # 有些图带白底，把接近纯白的边角抠掉，让托盘图标背景透明
    if src.getextrema()[3][0] == 255:   # 完全不透明 => 可能是白底
        px = src.load()
        w, h = src.size
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if r > 246 and g > 246 and b > 246:
                    px[x, y] = (r, g, b, 0)
        print("  （已抠掉白色背景）")

    print("生成图标:")
    save_ico(src, os.path.join(OUT_DIR, "connected.ico"))
    save_ico(to_gray(src), os.path.join(OUT_DIR, "idle.ico"))
    save_ico(src, os.path.join(OUT_DIR, "app.ico"))

    # 预览图，方便肉眼检查
    src.resize((128, 128), Image.LANCZOS).save(
        os.path.join(OUT_DIR, "preview_connected.png"))
    to_gray(src).resize((128, 128), Image.LANCZOS).save(
        os.path.join(OUT_DIR, "preview_idle.png"))

    print("完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
