# -*- coding: utf-8 -*-
"""
生成托盘图标 —— AirPlay 风格：圆角方框 + 底部三角 + 上方声波。
两种状态：
  connected  绿色（#2ECC71）+ 白色符号
  idle       灰色（#8A8F98）+ 白色符号

输出 16/20/24/32 px 的 ICO，写到 tray_icons/ 下。
"""
import os
from PIL import Image, ImageDraw

OUT_DIR = "tray_icons"


def make_icon(accent, size=256):
    """画一张大图（后面缩放出各尺寸）。"""
    S = size
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # --- 圆角方框背景 ---
    pad = int(S * 0.06)
    radius = int(S * 0.22)
    d.rounded_rectangle([pad, pad, S - pad, S - pad],
                        radius=radius, fill=accent)

    # --- AirPlay 符号：底部梯形（屏幕）+ 三角形（播放头）+ 上方弧线（信号） ---
    white = (255, 255, 255, 255)

    # 屏幕/底座：一个下方收窄的矩形
    scr_w = int(S * 0.46)
    scr_h = int(S * 0.26)
    scr_x = (S - scr_w) // 2
    scr_y = int(S * 0.30)
    d.rectangle([scr_x, scr_y, scr_x + scr_w, scr_y + scr_h], fill=white)

    # 底座小脚
    foot_w = int(S * 0.14)
    foot_h = int(S * 0.05)
    d.rectangle([(S - foot_w) // 2, scr_y + scr_h,
                 (S + foot_w) // 2, scr_y + scr_h + foot_h], fill=white)

    # 播放三角（自下而上，指向屏幕）
    tri_w = int(S * 0.44)
    tri_h = int(S * 0.22)
    tri_top_y = scr_y + scr_h + foot_h + int(S * 0.02)
    d.polygon([
        ((S - tri_w) // 2, tri_top_y + tri_h),
        ((S + tri_w) // 2, tri_top_y + tri_h),
        (S // 2,           tri_top_y),
    ], fill=white)

    # 两道弧线（信号）
    for i, scale in enumerate([0.30, 0.40]):
        r = int(S * scale)
        lw = max(2, int(S * 0.035))
        cx, cy = S // 2, int(S * 0.44)
        d.arc([cx - r, cy - r, cx + r, cy + r],
              start=200, end=340, fill=white, width=lw)

    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    colors = {
        "connected": (46, 204, 113, 255),    # 绿
        "idle":      (138, 143, 152, 255),   # 灰
    }

    sizes = [16, 20, 24, 32, 48, 256]

    for name, accent in colors.items():
        big = make_icon(accent, 256)
        frames = [big.resize((s, s), Image.LANCZOS) for s in sizes]
        ico_path = os.path.join(OUT_DIR, name + ".ico")
        frames[-1].save(ico_path, format="ICO",
                        sizes=[(s, s) for s in sizes])
        # 同时存一张 PNG 方便预览
        big.resize((128, 128), Image.LANCZOS).save(
            os.path.join(OUT_DIR, name + "_preview.png"))
        print("wrote " + ico_path)


if __name__ == "__main__":
    main()
