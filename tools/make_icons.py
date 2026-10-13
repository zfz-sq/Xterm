# -*- coding: utf-8 -*-
"""Generate Xterm icon assets (PNG toolbar icons + multi-size app.ico).

Toolbar icons are drawn twice: resources/icons/light and resources/icons/dark.
Neutral glyphs follow the theme foreground. The connect plug is white in both
sets so it stays readable on the green connect button.

All icons are drawn on a 256x256 canvas, then downscaled with LANCZOS.
Re-run after editing shapes:
    python tools/make_icons.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "resources", "icons")
S = 256  # canvas size
CX = CY = 128
TERMINAL_BG = (38, 50, 56, 255)  # #263238
TERMINAL_FG = (76, 175, 80, 255)  # #4CAF50

# neutral: 工具栏单色图标（齿轮、箭头、刷新等）
# accent:  添加
# danger:  删除 / 断开斜杠
# on_green: 连接按钮图标，两套主题都是白，压在 #2EA043 上
PALETTES = {
    "light": {
        "neutral": (58, 63, 70, 255),     # #3A3F46
        "accent": (22, 120, 58, 255),     # #16783A
        "danger": (208, 69, 59, 255),     # #D0453B 与断开按钮文字同色
        "on_green": (255, 255, 255, 255),
    },
    "dark": {
        "neutral": (214, 214, 214, 255),  # #D6D6D6
        "accent": (62, 168, 84, 255),     # #3EA854
        "danger": (244, 92, 92, 255),     # #F45C5C
        "on_green": (255, 255, 255, 255),
    },
}

LEGACY = [
    "add.png", "chevron_left.png", "chevron_right.png", "connect.png",
    "copy.png", "disconnect.png", "down.png", "gear.png", "grip.png",
    "moon.png", "refresh.png", "remove.png", "send.png", "sun.png",
    "trash.png", "up.png",
]


def canvas():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def rline(d, p1, p2, width, color, cap=True):
    """Line with rounded caps."""
    d.line([p1, p2], fill=color, width=width)
    if cap:
        r = width / 2.0
        for p in (p1, p2):
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def rpoly(d, pts, width, color, close=True):
    """Polyline with rounded joints."""
    segs = list(zip(pts, pts[1:]))
    if close:
        segs.append((pts[-1], pts[0]))
    for a, b in segs:
        rline(d, a, b, width, color)
    if close:
        r = width / 2.0
        for p in pts:
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def save(img, theme, name, size=32):
    folder = os.path.join(OUT_DIR, theme)
    os.makedirs(folder, exist_ok=True)
    img.resize((size, size), Image.LANCZOS).save(os.path.join(folder, name))
    print("wrote", theme + "/" + name)


def draw_toolbar(theme, c):
    neutral = c["neutral"]
    accent = c["accent"]
    danger = c["danger"]
    on_green = c["on_green"]

    # ---- refresh (circular arrow) ----
    img, d = canvas()
    r = 76
    d.arc([CX - r, CY - r, CX + r, CY + r], start=40, end=320, fill=neutral, width=22)
    ex = CX + r * math.cos(math.radians(320))
    ey = CY + r * math.sin(math.radians(320))
    tan = (math.cos(math.radians(50)), math.sin(math.radians(50)))
    nrm = (math.cos(math.radians(140)), math.sin(math.radians(140)))
    tip = (ex + tan[0] * 52, ey + tan[1] * 52)
    b1 = (ex - tan[0] * 8 + nrm[0] * 26, ey - tan[1] * 8 + nrm[1] * 26)
    b2 = (ex - tan[0] * 8 - nrm[0] * 26, ey - tan[1] * 8 - nrm[1] * 26)
    d.polygon([tip, b1, b2], fill=neutral)
    save(img, theme, "refresh.png")

    # ---- connect (power plug, white so it reads on the green button) ----
    img, d = canvas()
    rline(d, (100, 58), (100, 112), 20, on_green)
    rline(d, (156, 58), (156, 112), 20, on_green)
    d.rounded_rectangle([80, 112, 176, 176], radius=22, outline=on_green, width=20)
    rline(d, (128, 176), (128, 206), 20, on_green)
    save(img, theme, "connect.png")

    # ---- disconnect (plug + slash) ----
    img, d = canvas()
    rline(d, (100, 58), (100, 112), 20, neutral)
    rline(d, (156, 58), (156, 112), 20, neutral)
    d.rounded_rectangle([80, 112, 176, 176], radius=22, outline=neutral, width=20)
    rline(d, (128, 176), (128, 206), 20, neutral)
    rline(d, (56, 56), (200, 200), 26, danger)
    save(img, theme, "disconnect.png")

    # ---- send (paper plane) ----
    img, d = canvas()
    d.polygon([(44, 52), (212, 128), (44, 204), (86, 128)], fill=neutral)
    save(img, theme, "send.png")

    # ---- add ----
    img, d = canvas()
    rline(d, (128, 66), (128, 190), 24, accent)
    rline(d, (66, 128), (190, 128), 24, accent)
    save(img, theme, "add.png")

    # ---- remove ----
    img, d = canvas()
    rline(d, (66, 128), (190, 128), 24, danger)
    save(img, theme, "remove.png")

    # ---- up ----
    img, d = canvas()
    rpoly(d, [(66, 152), (128, 90), (190, 152)], 22, neutral, close=False)
    save(img, theme, "up.png")

    # ---- down ----
    img, d = canvas()
    rpoly(d, [(66, 104), (128, 166), (190, 104)], 22, neutral, close=False)
    save(img, theme, "down.png")

    # ---- copy (two sheets). Front sheet stays white so the back outline shows through. ----
    img, d = canvas()
    d.rounded_rectangle([96, 56, 196, 152], radius=16, outline=neutral, width=16)
    d.rounded_rectangle([60, 92, 160, 196], radius=16, outline=neutral, width=16,
                        fill=(255, 255, 255, 255))
    d.rounded_rectangle([60, 92, 160, 196], radius=16, outline=neutral, width=16)
    save(img, theme, "copy.png")

    # ---- grip (drag handle) ----
    img, d = canvas()
    for y in (86, 128, 170):
        for x in (104, 152):
            d.ellipse([x - 15, y - 15, x + 15, y + 15], fill=neutral)
    save(img, theme, "grip.png")

    # ---- gear ----
    img, d = canvas()
    for k in range(8):
        a = math.radians(k * 45)
        p1 = (CX + 58 * math.cos(a), CY + 58 * math.sin(a))
        p2 = (CX + 92 * math.cos(a), CY + 92 * math.sin(a))
        rline(d, p1, p2, 28, neutral)
    d.ellipse([CX - 66, CY - 66, CX + 66, CY + 66], outline=neutral, width=22)
    d.ellipse([CX - 20, CY - 20, CX + 20, CY + 20], outline=neutral, width=18)
    save(img, theme, "gear.png")

    # ---- trash ----
    img, d = canvas()
    d.rounded_rectangle([112, 48, 144, 74], radius=8, outline=danger, width=14)
    rline(d, (70, 82), (186, 82), 18, danger)
    d.rounded_rectangle([84, 82, 172, 202], radius=18, outline=danger, width=18)
    rline(d, (114, 114), (114, 170), 12, danger)
    rline(d, (142, 114), (142, 170), 12, danger)
    save(img, theme, "trash.png")

    # ---- chevron right (collapse) ----
    img, d = canvas()
    rpoly(d, [(100, 66), (162, 128), (100, 190)], 22, neutral, close=False)
    save(img, theme, "chevron_right.png")

    # ---- chevron left ----
    img, d = canvas()
    rpoly(d, [(156, 66), (94, 128), (156, 190)], 22, neutral, close=False)
    save(img, theme, "chevron_left.png")

    # ---- sun (shown while the app is dark: "switch to light") ----
    img, d = canvas()
    r = 42
    d.ellipse([CX - r, CY - r, CX + r, CY + r], outline=neutral, width=18)
    for k in range(8):
        a = math.radians(k * 45)
        rline(d, (CX + 62 * math.cos(a), CY + 62 * math.sin(a)),
              (CX + 88 * math.cos(a), CY + 88 * math.sin(a)), 16, neutral)
    save(img, theme, "sun.png")

    # ---- ssh (钥匙: 模式切换按钮，切到 SSH 终端) ----
    img, d = canvas()
    d.ellipse([40, 86, 112, 158], outline=neutral, width=18)
    rline(d, (110, 122), (204, 122), 18, neutral)
    rline(d, (170, 122), (170, 166), 16, neutral)
    rline(d, (196, 122), (196, 152), 16, neutral)
    save(img, theme, "ssh.png")

    # ---- serial (D-sub 梯形连接器: 切回串口终端) ----
    img, d = canvas()
    shell = [(52, 92), (204, 92), (176, 166), (80, 166)]
    rpoly(d, shell, 18, neutral, close=True)
    for x in (99, 128, 157):
        d.ellipse([x - 11, 104, x + 11, 126], fill=neutral)
    for x in (113, 143):
        d.ellipse([x - 11, 130, x + 11, 152], fill=neutral)
    save(img, theme, "serial.png")

    # ---- moon (shown while the app is light: "switch to dark") ----
    img, d = canvas()
    r = 66
    d.ellipse([CX - r, CY - r, CX + r, CY + r], fill=neutral)
    cut = Image.new("L", (S, S), 0)
    ImageDraw.Draw(cut).ellipse([CX - r + 58, CY - r - 26, CX + r + 58, CY + r - 26], fill=255)
    alpha = ImageChops.subtract(img.getchannel("A"), cut)
    img.putalpha(alpha)
    save(img, theme, "moon.png")

    # ---- folder (SFTP 文件浏览器入口) ----
    img, d = canvas()
    d.polygon([(44, 76), (104, 76), (124, 96), (212, 96)], fill=neutral)
    d.polygon([(44, 76), (44, 188), (212, 188), (212, 96)], fill=neutral)
    save(img, theme, "folder.png")

    # ---- pencil (重命名) ----
    img, d = canvas()
    d.polygon([(140, 56), (200, 116), (104, 212), (44, 212), (44, 152)], fill=neutral)
    d.polygon([(160, 36), (188, 64), (148, 104), (120, 76)], fill=neutral)
    save(img, theme, "pencil.png")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for theme, palette in PALETTES.items():
        draw_toolbar(theme, palette)

    for name in LEGACY:
        path = os.path.join(OUT_DIR, name)
        if os.path.isfile(path):
            os.remove(path)
            print("removed legacy", name)

    # ---- app icon (not themed) ----
    img, d = canvas()
    d.rounded_rectangle([14, 14, 242, 242], radius=52, fill=TERMINAL_BG)
    rpoly(d, [(66, 82), (124, 128), (66, 174)], 20, TERMINAL_FG, close=False)
    rline(d, (126, 190), (192, 190), 20, TERMINAL_FG)
    img.resize((64, 64), Image.LANCZOS).save(os.path.join(OUT_DIR, "app.png"))
    img.resize((256, 256), Image.LANCZOS).save(
        os.path.join(OUT_DIR, "app_256.png"))
    print("wrote app.png / app_256.png")

    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(os.path.join(HERE, "..", "resources", "app.ico"), sizes=ico_sizes)
    print("wrote app.ico")


if __name__ == "__main__":
    main()
