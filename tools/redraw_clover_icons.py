#!/usr/bin/env python3
"""Redraw ic_clover_*.png to match home_clover_round gold-standard line art."""
from __future__ import annotations

import math
import os

import cairocffi as cairo
from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "main", "xingzhi-assets")
OUT_SIZE = 64
DRAW_SIZE = 256
STROKE = 10.5  # on 256 canvas ≈ 2.6px at 64


def new_ctx():
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, DRAW_SIZE, DRAW_SIZE)
    cr = cairo.Context(surf)
    cr.set_operator(cairo.OPERATOR_CLEAR)
    cr.paint()
    cr.set_operator(cairo.OPERATOR_OVER)
    cr.set_source_rgba(1, 1, 1, 1)
    cr.set_line_width(STROKE)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_join(cairo.LINE_JOIN_ROUND)
    return surf, cr


def stroke(cr):
    cr.stroke()


def fill(cr):
    cr.fill()


def save(surf, name: str):
    tmp = f"/tmp/_clover_{name}.png"
    surf.write_to_png(tmp)
    im = Image.open(tmp).convert("RGBA")
    out = im.resize((OUT_SIZE, OUT_SIZE), Image.Resampling.LANCZOS)
    # keep only near-white strokes, drop dark residuals
    px = out.load()
    for y in range(OUT_SIZE):
        for x in range(OUT_SIZE):
            r, g, b, a = px[x, y]
            if a < 18:
                px[x, y] = (0, 0, 0, 0)
                continue
            lum = (r + g + b) / 3
            if lum < 40:
                px[x, y] = (0, 0, 0, 0)
            else:
                px[x, y] = (255, 255, 255, a)
    path = os.path.abspath(os.path.join(ROOT, f"ic_clover_{name}.png"))
    out.save(path)
    print("wrote", path)


def rounded_rect(cr, x, y, w, h, r):
    cr.new_path()
    cr.move_to(x + r, y)
    cr.line_to(x + w - r, y)
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.line_to(x + w, y + h - r)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.line_to(x + r, y + h)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.line_to(x, y + r)
    cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()


def circle(cr, cx, cy, r, do_stroke=True):
    cr.new_sub_path()
    cr.arc(cx, cy, r, 0, 2 * math.pi)
    if do_stroke:
        stroke(cr)


def icon_chat(cr):
    rounded_rect(cr, 52, 48, 152, 108, 28)
    stroke(cr)
    # tail
    cr.move_to(86, 150)
    cr.line_to(62, 196)
    cr.line_to(118, 156)
    stroke(cr)
    for x in (92, 128, 164):
        cr.new_sub_path()
        cr.arc(x, 102, 9.5, 0, 2 * math.pi)
        fill(cr)


def icon_wifi(cr):
    cx, cy, r = 128, 128, 78
    circle(cr, cx, cy, r)
    # equator
    half0 = r - 4
    cr.move_to(cx - half0, cy)
    cr.line_to(cx + half0, cy)
    stroke(cr)
    # latitudes
    for dy, bulge in ((-40, 20), (40, -20)):
        y = cy + dy
        half = math.sqrt(max(1.0, r * r - dy * dy)) - 6
        cr.move_to(cx - half, y)
        cr.curve_to(cx - half * 0.32, y + bulge, cx + half * 0.32, y + bulge, cx + half, y)
        stroke(cr)
    # vertical meridian
    cr.move_to(cx, cy - r + 4)
    cr.line_to(cx, cy + r - 4)
    stroke(cr)
    # side meridians
    cr.new_sub_path()
    cr.save()
    cr.translate(cx, cy)
    cr.scale(0.46, 1.0)
    cr.arc(0, 0, r, 0, 2 * math.pi)
    cr.restore()
    stroke(cr)


def icon_recording(cr):
    cx = 128
    rounded_rect(cr, 104, 40, 48, 92, 24)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(cx, 116, 46, 0.12 * math.pi, 0.88 * math.pi)
    stroke(cr)
    cr.move_to(cx, 162)
    cr.line_to(cx, 196)
    stroke(cr)
    cr.move_to(98, 200)
    cr.line_to(158, 200)
    stroke(cr)


def icon_music(cr):
    cr.save()
    cr.translate(128, 128)
    cr.rotate(math.radians(-14))
    cr.translate(-128, -128)
    cr.new_sub_path()
    cr.arc(86, 184, 22, 0, 2 * math.pi)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(170, 168, 22, 0, 2 * math.pi)
    stroke(cr)
    cr.move_to(108, 184)
    cr.line_to(108, 60)
    stroke(cr)
    cr.move_to(192, 168)
    cr.line_to(192, 48)
    stroke(cr)
    cr.move_to(108, 60)
    cr.line_to(192, 48)
    stroke(cr)
    cr.move_to(108, 78)
    cr.line_to(192, 66)
    stroke(cr)
    cr.restore()


def icon_camera(cr):
    rounded_rect(cr, 48, 92, 160, 100, 22)
    stroke(cr)
    rounded_rect(cr, 70, 70, 50, 26, 8)
    stroke(cr)
    circle(cr, 128, 142, 30)


def icon_calendar(cr):
    rounded_rect(cr, 52, 60, 152, 144, 18)
    stroke(cr)
    cr.move_to(52, 108)
    cr.line_to(204, 108)
    stroke(cr)
    for x in (88, 128, 168):
        cr.move_to(x, 48)
        cr.line_to(x, 78)
        stroke(cr)
    for y in (136, 168):
        for x in (84, 128, 172):
            cr.new_sub_path()
            cr.arc(x, y, 6, 0, 2 * math.pi)
            fill(cr)


def icon_calculator(cr):
    rounded_rect(cr, 56, 44, 144, 168, 20)
    stroke(cr)
    rounded_rect(cr, 76, 64, 104, 36, 10)
    stroke(cr)
    for i, (x, y) in enumerate(((84, 124), (128, 124), (172, 124), (84, 168), (128, 168), (172, 168))):
        cr.new_sub_path()
        cr.arc(x, y, 8, 0, 2 * math.pi)
        fill(cr)


def icon_weather(cr):
    sx, sy, sr = 96, 92, 30
    circle(cr, sx, sy, sr)
    for ang in range(0, 360, 45):
        rad = math.radians(ang)
        cr.move_to(sx + math.cos(rad) * 42, sy + math.sin(rad) * 42)
        cr.line_to(sx + math.cos(rad) * 56, sy + math.sin(rad) * 56)
        stroke(cr)
    cr.new_path()
    cr.move_to(58, 196)
    cr.line_to(200, 196)
    cr.curve_to(220, 196, 220, 164, 198, 158)
    cr.curve_to(206, 128, 164, 116, 146, 140)
    cr.curve_to(136, 112, 88, 114, 80, 146)
    cr.curve_to(52, 148, 44, 180, 58, 196)
    cr.close_path()
    stroke(cr)


def icon_sd(cr):
    cr.move_to(78, 56)
    cr.line_to(168, 56)
    cr.line_to(196, 88)
    cr.line_to(196, 204)
    cr.line_to(60, 204)
    cr.line_to(60, 88)
    cr.close_path()
    stroke(cr)
    for i, x in enumerate((88, 112, 136, 160)):
        cr.move_to(x, 78)
        cr.line_to(x, 108)
        stroke(cr)


def icon_settings(cr):
    cx, cy = 128, 128
    teeth = 8
    r_tip = 82
    r_root = 58
    r_hub = 26
    cr.new_path()
    for i in range(teeth):
        a = i * 2 * math.pi / teeth
        half = math.pi / teeth
        tooth_w = half * 0.38
        a0 = a - half
        a1 = a - tooth_w
        a2 = a + tooth_w
        a3 = a + half
        if i == 0:
            cr.move_to(cx + r_root * math.cos(a0), cy + r_root * math.sin(a0))
        cr.arc(cx, cy, r_root, a0, a1)
        cr.line_to(cx + r_tip * math.cos(a1), cy + r_tip * math.sin(a1))
        cr.arc(cx, cy, r_tip, a1, a2)
        cr.line_to(cx + r_root * math.cos(a2), cy + r_root * math.sin(a2))
        cr.arc(cx, cy, r_root, a2, a3)
    cr.close_path()
    stroke(cr)
    circle(cr, cx, cy, r_hub)


def icon_radio(cr):
    rounded_rect(cr, 48, 96, 160, 96, 18)
    stroke(cr)
    cr.move_to(72, 96)
    cr.line_to(160, 48)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(160, 48, 7, 0, 2 * math.pi)
    fill(cr)
    for x in (88, 128, 168):
        cr.move_to(x, 128)
        cr.line_to(x, 168)
        stroke(cr)


def icon_2048(cr):
    rounded_rect(cr, 48, 48, 160, 160, 24)
    stroke(cr)
    cr.move_to(128, 48)
    cr.line_to(128, 208)
    stroke(cr)
    cr.move_to(48, 128)
    cr.line_to(208, 128)
    stroke(cr)


def icon_info(cr):
    circle(cr, 128, 128, 78)
    cr.new_sub_path()
    cr.arc(128, 86, 10, 0, 2 * math.pi)
    fill(cr)
    cr.move_to(128, 112)
    cr.line_to(128, 176)
    cr.set_line_width(STROKE + 4)
    stroke(cr)
    cr.set_line_width(STROKE)


def icon_theme(cr):
    circle(cr, 128, 128, 72)
    cr.move_to(128, 56)
    cr.line_to(128, 200)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(128, 128, 72, -math.pi / 2, math.pi / 2)
    cr.line_to(128, 56)
    fill(cr)


def icon_translate(cr):
    rounded_rect(cr, 40, 72, 100, 80, 18)
    stroke(cr)
    cr.move_to(72, 152)
    cr.line_to(56, 184)
    cr.line_to(96, 158)
    stroke(cr)
    rounded_rect(cr, 116, 104, 100, 80, 18)
    stroke(cr)
    cr.move_to(188, 184)
    cr.line_to(204, 216)
    cr.line_to(164, 190)
    stroke(cr)


def icon_ai_image_gen(cr):
    rounded_rect(cr, 48, 64, 160, 128, 18)
    stroke(cr)
    cr.move_to(64, 168)
    cr.line_to(108, 116)
    cr.line_to(140, 148)
    cr.line_to(168, 112)
    cr.line_to(196, 168)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(88, 100, 12, 0, 2 * math.pi)
    fill(cr)
    # spark
    cr.move_to(196, 56)
    cr.line_to(210, 40)
    stroke(cr)
    cr.move_to(196, 48)
    cr.line_to(216, 48)
    stroke(cr)
    cr.move_to(204, 56)
    cr.line_to(204, 36)
    stroke(cr)


def icon_gps(cr):
    cx, cy = 128, 108
    cr.new_path()
    cr.arc(cx, cy, 54, math.pi * 0.72, math.pi * 0.28)
    cr.line_to(cx, 214)
    cr.close_path()
    stroke(cr)
    circle(cr, cx, cy, 22)


def icon_call(cr):
    cr.save()
    cr.translate(128, 128)
    cr.rotate(math.radians(135))
    cr.translate(-128, -128)
    rounded_rect(cr, 104, 36, 48, 64, 20)
    stroke(cr)
    rounded_rect(cr, 104, 156, 48, 64, 20)
    stroke(cr)
    cr.move_to(112, 98)
    cr.line_to(112, 158)
    stroke(cr)
    cr.move_to(144, 98)
    cr.line_to(144, 158)
    stroke(cr)
    cr.restore()


def icon_spirit_level(cr):
    rounded_rect(cr, 40, 100, 176, 56, 28)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(128, 128, 18, 0, 2 * math.pi)
    stroke(cr)
    cr.move_to(96, 100)
    cr.line_to(96, 156)
    stroke(cr)
    cr.move_to(160, 100)
    cr.line_to(160, 156)
    stroke(cr)


def icon_magnet(cr):
    cr.new_path()
    cr.move_to(80, 56)
    cr.line_to(80, 140)
    cr.curve_to(80, 196, 176, 196, 176, 140)
    cr.line_to(176, 56)
    cr.line_to(148, 56)
    cr.line_to(148, 140)
    cr.curve_to(148, 164, 108, 164, 108, 140)
    cr.line_to(108, 56)
    cr.close_path()
    stroke(cr)
    cr.move_to(80, 88)
    cr.line_to(108, 88)
    stroke(cr)
    cr.move_to(148, 88)
    cr.line_to(176, 88)
    stroke(cr)


def icon_vibrate(cr):
    rounded_rect(cr, 88, 48, 80, 160, 18)
    stroke(cr)
    cr.move_to(128, 188)
    cr.line_to(128, 188)
    cr.new_sub_path()
    cr.arc(128, 72, 6, 0, 2 * math.pi)
    fill(cr)
    for i, off in enumerate((0, 16, 32)):
        cr.new_sub_path()
        cr.arc(88, 128, 28 + off, math.pi * 0.65, math.pi * 1.35)
        stroke(cr)
        cr.new_sub_path()
        cr.arc(168, 128, 28 + off, -math.pi * 0.35, math.pi * 0.35)
        stroke(cr)


def icon_pin(cr):
    rounded_rect(cr, 92, 48, 72, 160, 12)
    stroke(cr)
    for i in range(4):
        y = 76 + i * 36
        cr.move_to(92, y)
        cr.line_to(56, y)
        stroke(cr)
        cr.move_to(164, y)
        cr.line_to(200, y)
        stroke(cr)


def icon_test(cr):
    rounded_rect(cr, 68, 48, 120, 168, 16)
    stroke(cr)
    cr.move_to(96, 48)
    cr.line_to(96, 76)
    cr.line_to(160, 76)
    cr.line_to(160, 48)
    stroke(cr)
    cr.move_to(92, 128)
    cr.line_to(116, 152)
    cr.line_to(168, 100)
    stroke(cr)


def icon_digital_people(cr):
    circle(cr, 128, 92, 36)
    cr.new_sub_path()
    cr.arc(128, 220, 72, math.pi * 1.08, math.pi * 1.92)
    stroke(cr)


def icon_espclaw(cr):
    # simple claw / pincer
    cr.move_to(128, 48)
    cr.line_to(128, 100)
    stroke(cr)
    cr.new_path()
    cr.move_to(128, 100)
    cr.curve_to(64, 100, 48, 176, 88, 208)
    cr.move_to(128, 100)
    cr.curve_to(192, 100, 208, 176, 168, 208)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(96, 196, 14, 0, 2 * math.pi)
    stroke(cr)
    cr.new_sub_path()
    cr.arc(160, 196, 14, 0, 2 * math.pi)
    stroke(cr)


ICONS = {
    "chat": icon_chat,
    "wifi": icon_wifi,
    "recording": icon_recording,
    "music": icon_music,
    "camera": icon_camera,
    "calendar": icon_calendar,
    "calculator": icon_calculator,
    "weather": icon_weather,
    "sd": icon_sd,
    "settings": icon_settings,
    "radio": icon_radio,
    "2048": icon_2048,
    "info": icon_info,
    "theme": icon_theme,
    "translate": icon_translate,
    "ai_image_gen": icon_ai_image_gen,
    "gps": icon_gps,
    "call": icon_call,
    "spirit_level": icon_spirit_level,
    "magnet": icon_magnet,
    "vibrate": icon_vibrate,
    "pin": icon_pin,
    "test": icon_test,
    "digital_people": icon_digital_people,
    "espclaw": icon_espclaw,
}


def main():
    os.makedirs(os.path.abspath(ROOT), exist_ok=True)
    for name, fn in ICONS.items():
        surf, cr = new_ctx()
        fn(cr)
        save(surf, name)


if __name__ == "__main__":
    main()
