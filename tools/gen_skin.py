#!/usr/bin/env python3
"""Generates the hand-built MPC skin for Twisted Block into plugin/skins/.

Writes:
  plugin/skins/TUI.json      the touch screen (GRID, SETUP and SAMPLE tabs)
  plugin/skins/Q-Links.json  knob assignments
  plugin/skins/img/*.png     glyph film strips, lamps, buttons, meter bars

Style: black text on white, big, high contrast. Glyphs are drawn with
DejaVu Sans Mono Bold (Bitstream Vera licence, free to redistribute) or any
bold monospace font given with --font.

Keep the constants below in step with plugin/src/TwistedBlock.cpp.
"""
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---- must match TwistedBlock.cpp ---------------------------------------------
ROWS = 16
CENTRE_ROW = 8
COLS = 6
FIELDS = 5
LANES = 4
WAVE_BARS = 64

(P_VOLUME, P_LANE, P_CURSOR, P_FIELD, P_VALUE, P_FOLLOW, P_LPB, P_LENGTH, P_STRESS, P_REDRAW, P_PCOUNT, P_TOUCH,
 P_MODE, P_INST, P_OCTAVE, P_STEP, P_BLOCK, P_BASE, P_TRIGGER, P_QUANT, P_TICKS,
 P_FILE, P_LOAD, P_RESCAN, P_START, P_END, P_LOOP, P_LSTART, P_GAIN, P_REVERSE, P_NORMAL, P_SLMODE, P_SLICES, P_SENSE,
 P_SNAME, P_PLAYING) = range(36)
P_ROW_BASE = 36
P_CURSOR_SLOT = P_ROW_BASE + ROWS * COLS
P_PLAY_SLOT = P_CURSOR_SLOT + 1
P_ACT_BASE = P_PLAY_SLOT + 1
P_WAVE_BASE = P_ACT_BASE + LANES
P_START_SLOT = P_WAVE_BASE + WAVE_BARS
P_END_SLOT = P_START_SLOT + 1
P_LOOP_SLOT = P_END_SLOT + 1
P_SLICE_BASE = P_LOOP_SLOT + 1
NUM_PARAMS = P_SLICE_BASE + WAVE_BARS
TOUCH_IDLE = ROWS * FIELDS

# ---- layout (tab coordinates, 1280 x 628) -------------------------------------
HEADER_H = 40
ROW_Y0 = 44
ROW_H = 36
GRID_W = 540
FONT_PX = 30
# x, width for: line, note, inst, vol, cmd, val
COL_BOX = [(10, 52), (84, 84), (196, 60), (282, 60), (368, 34), (428, 60)]
COL_TITLES = ["LN", "NOTE", "INS", "VOL", "C", "VAL"]

PANEL_X = 572
BLACK = (0, 0, 0, 255)
CLEAR = (0, 0, 0, 0)
WHITE = (255, 255, 255, 255)
GREY = (190, 190, 190, 255)

def hex2(v):
    return "%02X" % (v & 0xFF)


def strip_frames():
    names = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
    line = [hex2(i) for i in range(128)] + [""]
    note = ["---", "OFF"] + ["%s%d" % (names[n % 12], n // 12) for n in range(120)] + [""]
    inst = ["--"] + [hex2(i) for i in range(1, 256)] + [""]
    vol = ["--"] + [hex2(i) for i in range(65)] + [""]
    cmd = ["-"] + [str(i) for i in range(10)] + [chr(ord("A") + i) for i in range(26)] + [""]
    val = [hex2(i) for i in range(256)] + ["--", ""]
    return [("line", line), ("note", note), ("inst", inst), ("vol", vol), ("cmd", cmd), ("val", val)]


EXPECTED_FRAMES = [129, 123, 257, 67, 38, 258]


FONT_FILE = None


class Gen:
    def __init__(self, out, font_path):
        global FONT_FILE
        FONT_FILE = font_path
        self.out = out
        self.img = os.path.join(out, "img")
        os.makedirs(self.img, exist_ok=True)
        self.font = ImageFont.truetype(font_path, FONT_PX)
        self.big = ImageFont.truetype(font_path, 44)

    def save(self, im, name):
        im.save(os.path.join(self.img, name))
        return "img/" + name

    # ---- images ----------------------------------------------------------------
    def glyph_strip(self, name, frames, w, h):
        im = Image.new("RGBA", (w, h * len(frames)), CLEAR)
        d = ImageDraw.Draw(im)
        # Centre every frame on the height of a digit, so "-" sits where it would in "C-3".
        _, ref_t, _, ref_b = d.textbbox((0, 0), "0", font=self.font)
        for i, text in enumerate(frames):
            if not text:
                continue
            l, t, r, b = d.textbbox((0, 0), text, font=self.font)
            x = (w - (r - l)) / 2 - l
            y = i * h + (h - (ref_b - ref_t)) / 2 - ref_t
            d.text((x, y), text, font=self.font, fill=BLACK)
        return self.save(im, "strip_%s.png" % name)

    def solid(self, name, w, h, colour):
        return self.save(Image.new("RGBA", (w, h), colour), name)

    def cursor_bar(self):
        return self.solid("cursor_on.png", GRID_W, ROW_H, (200, 200, 200, 255))

    def play_marker(self):
        im = Image.new("RGBA", (GRID_W, ROW_H), CLEAR)
        d = ImageDraw.Draw(im)
        d.polygon([(0, 2), (10, ROW_H // 2), (0, ROW_H - 3)], fill=BLACK)
        d.polygon([(GRID_W - 1, 2), (GRID_W - 11, ROW_H // 2), (GRID_W - 1, ROW_H - 3)], fill=BLACK)
        d.rectangle([12, ROW_H - 4, GRID_W - 13, ROW_H - 1], fill=BLACK)
        return self.save(im, "play_on.png")

    def centre_lines(self):
        im = Image.new("RGBA", (GRID_W, ROW_H + 4), CLEAR)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, GRID_W - 1, 1], fill=(120, 120, 120, 255))
        d.rectangle([0, ROW_H + 2, GRID_W - 1, ROW_H + 3], fill=(120, 120, 120, 255))
        return self.save(im, "centre_lines.png")

    def lane_button(self, lane, on):
        w, h = 110, 66
        im = Image.new("RGBA", (w, h), (190, 190, 190, 255) if on else WHITE)
        d = ImageDraw.Draw(im)
        bw = 5 if on else 2
        d.rectangle([0, 0, w - 1, h - 1], outline=BLACK if on else (140, 140, 140, 255), width=bw)
        text = "L%d" % lane
        l, t, r, b = d.textbbox((0, 0), text, font=self.big)
        d.text(((w - (r - l)) / 2 - l, (h - (b - t)) / 2 - t), text, font=self.big, fill=BLACK)
        return self.save(im, "lane%d_%s.png" % (lane, "on" if on else "off"))

    def act_lamp(self, on):
        w, h = 110, 12
        im = Image.new("RGBA", (w, h), BLACK if on else WHITE)
        if not on:
            ImageDraw.Draw(im).rectangle([0, 0, w - 1, h - 1], outline=(170, 170, 170, 255), width=2)
        return self.save(im, "act_%s.png" % ("on" if on else "off"))

    def field_underline(self, w):
        im = Image.new("RGBA", (w, 6), BLACK)
        return self.save(im, "field_on_%d.png" % w)

    def clear(self, w, h):
        return self.solid("clear_%dx%d.png" % (w, h), w, h, CLEAR)

    def wave_frame(self, w, h):
        im = Image.new("RGBA", (w, h), CLEAR)
        ImageDraw.Draw(im).rectangle([0, 0, w - 1, h - 1], outline=(120, 120, 120, 255), width=2)
        return self.save(im, "wave_frame.png")


# ---- JSON helpers ------------------------------------------------------------------
def bg(colour="0"):
    return {"version": 1,
            "focussed": {"version": 1, "colour": colour, "image": ""},
            "unfocussed": {"version": 1, "colour": colour, "image": ""}}


def bounds(rect, focus="No", show="Show", when="Always"):
    return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": show,
            "whenVisible": when, "boundsType": "Absolute", "bounds": rect}


def rect(x, y, w, h):
    return "%g %g %g %g" % (x, y, w, h)


def entry(name, type_, data, rect_, remap=None, focus="No", show="Show", when="Always"):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": type_, "data": data},
            "handle remapping": {"version": 1, "map": remap or []},
            "bounds": bounds(rect_, focus, show, when)}


def text_style(height, colour="ff000000", style="Bold", just="left verticallyCentred", font="Roboto"):
    return {"version": 1, "font": {"version": 1, "name": font, "style": style, "height": float(height)},
            "colour": colour, "justification": just, "case": "Original"}


def label_text(text, height, just="left verticallyCentred"):
    return {"version": 1, "textStyle": text_style(height, just=just), "type": "Text", "handleName": "Invalid", "text": text}


def p(i):
    return "Parameter %d" % i


def component(children, actions=None, background="0", ignore=False):
    return {"version": 2, "actions": actions or [], "backgroundData": bg(background),
            "ignoreMousePresses": ignore, "disableCoarseDataWheel": False, "componentsData": children}


def ctrl_slot(overlay, w, h):
    """A focusable control: focus ring, name, big value. Touch selects its Q-Link."""
    actions = [
        {"version": 1, "onAction": "Mouse Down", "handler": "Q-Link", "handleName": "Data", "additionalData": ""},
        {"version": 1, "onAction": "Double Click", "handler": "Show Overlay", "handleName": "Data", "additionalData": overlay},
        {"version": 1, "onAction": "Enter Pressed", "handler": "Show Overlay", "handleName": "Data", "additionalData": overlay},
    ]
    kids = [
        entry("focus", "Focus", {"version": 1, "backgroundColour": "19000000", "outlineColour": "ff000000",
                                 "backgroundInset": 2.0, "outlineThickness": 4.0},
              rect(0, 0, w, h), when="WhenFocussed"),
        entry("name", "Label", {"version": 1, "textStyle": text_style(22, colour="ff333333"),
                                "type": "Name", "handleName": "Data"}, rect(12, 4, w - 20, 28)),
        entry("value", "Label", {"version": 1, "textStyle": text_style(34), "type": "Value", "handleName": "Data"},
              rect(12, 32, w - 20, h - 36)),
    ]
    return component(kids, actions)


def knob_overlay():
    kid = entry("overlay", "KnobOverlay", {
        "version": 2, "__DUPV__": 1,
        "actions": [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}],
        "overlayType": "Knob With Header", "scaleType": "Linear", "polarityType": "Unipolar",
        "showMinusSign": True, "handleName": "Data"}, rect(0, 0, 1280, 800), focus="Yes")
    return component([kid], ignore=True)


def menu_overlay():
    kid = entry("overlay", "MenuOverlay", {
        "version": 1, "__DUPV__": 1,
        "actions": [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}],
        "withNumbers": False, "handleName": "Data"}, rect(0, 0, 1280, 800), focus="Yes")
    return component([kid], ignore=True)


# ---- extra images --------------------------------------------------------------------
def text_button(g, name, label, on, w=200, h=60, size=26):
    im = Image.new("RGBA", (w, h), GREY if on else WHITE)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h - 1], outline=BLACK if on else (120, 120, 120, 255), width=5 if on else 3)
    f = ImageFont.truetype(FONT_FILE, size)
    l, t, r, b = d.textbbox((0, 0), label, font=f)
    d.text(((w - (r - l)) / 2 - l, (h - (b - t)) / 2 - t), label, font=f, fill=BLACK)
    return g.save(im, "btn_%s_%s.png" % (name, "on" if on else "off"))


def letter_marker(g, letter, w, h, point_down):
    im = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT_FILE, 20)
    l, t, r, b = d.textbbox((0, 0), letter, font=f)
    ty = 0 if point_down else h - (b - t) - 2
    d.text((1 - l, ty - t), letter, font=f, fill=BLACK)
    # a line pointing at the waveform
    if point_down:
        d.rectangle([0, h - 6, 2, h - 1], fill=BLACK)
    else:
        d.rectangle([0, 0, 2, 5], fill=BLACK)
    return g.save(im, "mark_%s.png" % letter)


def slice_line(g, w, h):
    # Black|white|black line so it shows on white background and on black bars.
    im = Image.new("RGBA", (w, h), CLEAR)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 0, h - 1], fill=BLACK)
    d.rectangle([1, 0, 2, h - 1], fill=WHITE)
    d.rectangle([3, 0, 3, h - 1], fill=BLACK)
    d.polygon([(0, 0), (8, 0), (0, 8)], fill=BLACK)
    d.polygon([(0, h - 1), (8, h - 1), (0, h - 9)], fill=BLACK)
    return g.save(im, "slice_line.png")


def waveform(g, kids, prefix, x, y, bw, h, with_markers):
    bar_off = g.solid("bar_off_%d_%d.png" % (bw, h), bw, h, (238, 238, 238, 255))
    im = Image.new("RGBA", (bw, h), CLEAR)
    ImageDraw.Draw(im).rectangle([0, 0, bw - 2, h - 1], fill=BLACK)
    bar_on = g.save(im, "bar_on_%d_%d.png" % (bw, h))
    for b in range(WAVE_BARS):
        kids.append(entry("%swave%02d" % (prefix, b), "Meter",
                          {"version": 3, "direction": "Up", "invert": False, "inactiveImage": bar_off,
                           "peakImage": bar_on, "rmsImage": bar_on, "peakHandle": p(P_WAVE_BASE + b),
                           "rmsHandle": p(P_WAVE_BASE + b), "peakHoldHandle": p(P_WAVE_BASE + b),
                           "peakHoldSize": 1, "peakHoldColour": "ff000000"},
                          rect(x + b * bw, y, bw, h)))
    frame = Image.new("RGBA", (WAVE_BARS * bw + 8, h + 8), CLEAR)
    ImageDraw.Draw(frame).rectangle([0, 0, WAVE_BARS * bw + 7, h + 7], outline=(120, 120, 120, 255), width=2)
    kids.append(entry(prefix + "waveframe", "Image", {"version": 2, "imageType": "Regular", "colour": "0",
                                                      "image": g.save(frame, "wave_frame_%d_%d.png" % (bw, h))},
                      rect(x - 4, y - 4, WAVE_BARS * bw + 8, h + 8)))
    if not with_markers:
        return
    line = slice_line(g, bw, h + 8)
    clear_line = g.clear(bw, h + 8)
    for b in range(WAVE_BARS):
        kids.append(entry("%sslice%02d" % (prefix, b), "Indicator",
                          {"version": 1, "onImage": line, "offImage": clear_line, "indicatorId": 1,
                           "numIndicatorsInGroup": 2, "handleName": p(P_SLICE_BASE + b)},
                          rect(x + b * bw, y - 4, bw, h + 8)))
    mw, mh = 18, 30
    m_s, m_e, m_l = letter_marker(g, "S", mw, mh, True), letter_marker(g, "E", mw, mh, True), letter_marker(g, "L", mw, mh, False)
    clear_m = g.clear(mw, mh)
    for b in range(WAVE_BARS):
        for name, img, slot, yy in (("ms", m_s, P_START_SLOT, y - 36), ("me", m_e, P_END_SLOT, y - 36), ("ml", m_l, P_LOOP_SLOT, y + h + 6)):
            # The end marker points at the right edge of its bar.
            xx = x + b * bw + (bw - 3 if name == "me" else 0)
            kids.append(entry("%s%s%02d" % (prefix, name, b), "Indicator",
                              {"version": 1, "onImage": img, "offImage": clear_m, "indicatorId": b,
                               "numIndicatorsInGroup": WAVE_BARS + 1, "handleName": p(slot)},
                              rect(xx, yy, mw, mh)))


def slots(kids, layout, x0, y0, sw, sh, gapx=6, gapy=4):
    """layout: list of columns, each a list of (param, kind). Column = one Q-Link bank."""
    for col, column in enumerate(layout):
        for row, item in enumerate(column):
            if item is None:
                continue
            param, kind = item
            kids.append(entry("ctrl%d_%d_%d" % (param, col, row), "slot_%s_%d" % (kind, sw), {"version": 1, "handleName": "Data"},
                              rect(x0 + col * (sw + gapx), y0 + row * (sh + gapy), sw, sh),
                              remap=[{"key": "Data", "value": p(param)}], focus="Yes", show="Hide"))


def value_label(kids, name, param, x, y, w, h, size, title=None, title_w=0):
    if title:
        kids.append(entry(name + "_t", "Label", label_text(title, 20), rect(x, y, title_w, h)))
    kids.append(entry(name, "Label", {"version": 1, "textStyle": text_style(size), "type": "Value", "handleName": p(param)},
                      rect(x + title_w, y, w, h)))


# ---- tabs ------------------------------------------------------------------------------
def grid_tab(g):
    kids = []
    for c, (x, w) in enumerate(COL_BOX):
        kids.append(entry("title%d" % c, "Label", label_text(COL_TITLES[c], 22, "horizontallyCentred verticallyCentred"),
                          rect(x - 6, 0, w + 12, HEADER_H - 8)))
    for f in range(FIELDS):
        x, w = COL_BOX[f + 1]
        kids.append(entry("fieldmark%d" % f, "Indicator",
                          {"version": 1, "onImage": g.field_underline(w + 8), "offImage": g.clear(w + 8, 6),
                           "indicatorId": f, "numIndicatorsInGroup": FIELDS, "handleName": p(P_FIELD)},
                          rect(x - 4, HEADER_H - 8, w + 8, 6)))
    clear_row = g.clear(GRID_W, ROW_H)
    cursor_on = g.cursor_bar()
    play_on = g.play_marker()
    for r in range(ROWS):
        y = ROW_Y0 + r * ROW_H
        kids.append(entry("cursor%02d" % r, "Indicator",
                          {"version": 1, "onImage": cursor_on, "offImage": clear_row, "indicatorId": r,
                           "numIndicatorsInGroup": ROWS + 1, "handleName": p(P_CURSOR_SLOT)},
                          rect(0, y, GRID_W, ROW_H)))
    kids.append(entry("centre", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": g.centre_lines()},
                      rect(0, ROW_Y0 + CENTRE_ROW * ROW_H - 2, GRID_W, ROW_H + 4)))
    strips = []
    for c, (name, frames) in enumerate(strip_frames()):
        assert len(frames) == EXPECTED_FRAMES[c], (name, len(frames))
        x, w = COL_BOX[c]
        strips.append((g.glyph_strip(name, frames, w, ROW_H), len(frames)))
    for r in range(ROWS):
        y = ROW_Y0 + r * ROW_H
        for c in range(COLS):
            x, w = COL_BOX[c]
            img, n = strips[c]
            kids.append(entry("r%02dc%d" % (r, c), "Image",
                              {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": img,
                               "numFrames": n, "handleName": p(P_ROW_BASE + r * COLS + c)},
                              rect(x, y, w, ROW_H)))
    for r in range(ROWS):
        y = ROW_Y0 + r * ROW_H
        kids.append(entry("play%02d" % r, "Indicator",
                          {"version": 1, "onImage": play_on, "offImage": clear_row, "indicatorId": r,
                           "numIndicatorsInGroup": ROWS + 1, "handleName": p(P_PLAY_SLOT)},
                          rect(0, y, GRID_W, ROW_H)))
    for r in range(ROWS):
        y = ROW_Y0 + r * ROW_H
        for f in range(FIELDS):
            x, w = COL_BOX[f + 1]
            img = g.clear(w + 16, ROW_H)
            kids.append(entry("touch%02d_%d" % (r, f), "Button",
                              {"version": 2, "onImage": img, "offImage": img, "buttonId": r * FIELDS + f,
                               "numButtonsInGroup": TOUCH_IDLE + 1, "handleName": p(P_TOUCH),
                               "gestureBehaviour": "Instant"},
                              rect(x - 8, y, w + 16, ROW_H)))
    # lanes
    act_on, act_off = g.act_lamp(True), g.act_lamp(False)
    for l in range(LANES):
        x = PANEL_X + l * 118
        kids.append(entry("lane%d" % l, "Button",
                          {"version": 2, "onImage": g.lane_button(l + 1, True), "offImage": g.lane_button(l + 1, False),
                           "buttonId": l, "numButtonsInGroup": LANES, "handleName": p(P_LANE), "gestureBehaviour": "Instant"},
                          rect(x, 4, 110, 66)))
        kids.append(entry("act%d" % l, "Indicator",
                          {"version": 1, "onImage": act_on, "offImage": act_off, "indicatorId": 1,
                           "numIndicatorsInGroup": 2, "handleName": p(P_ACT_BASE + l)},
                          rect(x, 74, 110, 12)))
    # info row: block, playing, sample
    value_label(kids, "info_block", P_BLOCK, PANEL_X, 96, 80, 48, 40, "BLOCK", 80)
    value_label(kids, "info_play", P_PLAYING, PANEL_X + 180, 96, 80, 48, 40, "PLAY", 64)
    value_label(kids, "info_sample", P_SNAME, PANEL_X + 330, 96, 260, 48, 26)
    kids.append(entry("info_line", "Image", {"version": 2, "imageType": "Regular", "colour": "0",
                                             "image": g.solid("hline.png", 450, 2, (150, 150, 150, 255))},
                      rect(PANEL_X, 150, 450, 2)))
    slots(kids, [[(P_CURSOR, "knob"), (P_FIELD, "menu"), (P_VALUE, "knob"), (P_LANE, "menu")],
                 [(P_MODE, "menu"), (P_INST, "knob"), (P_OCTAVE, "knob"), (P_STEP, "knob")],
                 [(P_BLOCK, "knob"), (P_LENGTH, "knob"), (P_LPB, "knob"), (P_FOLLOW, "menu")]],
          PANEL_X, 160, 146, 112)
    return component(kids, background="ffffffff")


COMMAND_HELP = [
    ("0xy", "Arpeggio +x +y"), ("1xx", "Slide up"), ("2xx", "Slide down"), ("3xx", "Glide to note"),
    ("4xy", "Vibrato speed/depth"), ("9xx", "Sample offset"), ("Axy", "Volume slide up/down"),
    ("Bxx", "Play backwards"), ("Cxx", "Set volume 00-40"), ("Dxx", "Delay note xx ticks"),
    ("Exx", "Chord (see below)"), ("Fxx", "Cut after xx ticks"), ("Rxx", "Retrigger every xx ticks"),
]


def setup_tab(g):
    kids = []
    slots(kids, [[(P_BASE, "knob"), (P_TRIGGER, "menu"), (P_QUANT, "menu"), (P_TICKS, "knob")],
                 [(P_VOLUME, "knob"), (P_REDRAW, "menu"), (P_LPB, "knob"), (P_FOLLOW, "menu")]],
          20, 16, 220, 112, gapx=10)
    kids.append(entry("stress", "Button", {"version": 2, "onImage": text_button(g, "stress", "STRESS TEST ON", True, 300, 56),
                                           "offImage": text_button(g, "stress", "STRESS TEST OFF", False, 300, 56),
                                           "buttonId": 1, "numButtonsInGroup": 2, "handleName": p(P_STRESS),
                                           "gestureBehaviour": "Toggle"},
                      rect(20, 500, 300, 56)))
    value_label(kids, "pcount", P_PCOUNT, 340, 500, 100, 56, 30, "PARAMS", 100)
    # Help text: how blocks map to keys, commands, chords.
    x0 = 500
    kids.append(entry("h_blocks", "Label", label_text("BLOCKS: Base Note = block 01, next key up = block 02 ...", 24),
                      rect(x0, 8, 770, 34)))
    kids.append(entry("h_mode", "Label", label_text("Play mode: keys/arranger trigger blocks.  Edit mode: pads enter notes.", 22),
                      rect(x0, 42, 770, 30)))
    kids.append(entry("h_cmd", "Label", label_text("COMMANDS (C column + value)", 24), rect(x0, 84, 770, 34)))
    for i, (code, text) in enumerate(COMMAND_HELP):
        col, row = divmod(i, 7)
        x = x0 + col * 390
        y = 120 + row * 40
        kids.append(entry("hc%d" % i, "Label", label_text(code, 26), rect(x, y, 70, 38)))
        kids.append(entry("ht%d" % i, "Label", label_text(text, 24), rect(x + 72, y, 310, 38)))
    kids.append(entry("h_chord", "Label", label_text("CHORDS Exx", 24), rect(x0, 410, 770, 34)))
    kids.append(entry("h_ch1", "Label", label_text("01 maj  02 min  03 7th  04 m7  05 maj7  06 sus2  07 sus4", 24),
                      rect(x0, 446, 770, 34)))
    kids.append(entry("h_ch2", "Label", label_text("08 dim  09 aug  0A 5th  0B octave  0C m9  0D add9", 24),
                      rect(x0, 482, 770, 34)))
    kids.append(entry("h_slice", "Label", label_text("SLICES: a sliced instrument plays slice n on C-3 + n", 24),
                      rect(x0, 530, 770, 34)))
    return component(kids, background="ffffffff")


def sample_tab(g):
    kids = []
    waveform(g, kids, "s_", 24, 48, 14, 200, True)
    # right panel: instrument, sample, file, buttons
    value_label(kids, "s_inst", P_INST, 940, 8, 100, 50, 44, "INST", 80)
    value_label(kids, "s_name", P_SNAME, 940, 60, 330, 44, 28)
    kids.append(entry("s_file_t", "Label", label_text("FILE TO LOAD", 20), rect(940, 110, 330, 26)))
    value_label(kids, "s_file", P_FILE, 940, 136, 330, 40, 24)
    for i, (param, name, label) in enumerate(((P_LOAD, "load", "LOAD"), (P_RESCAN, "rescan", "RESCAN"),
                                              (P_NORMAL, "normal", "NORMALISE"))):
        kids.append(entry("s_btn_" + name, "Button",
                          {"version": 2, "onImage": text_button(g, name, label, True, 300, 60),
                           "offImage": text_button(g, name, label, False, 300, 60),
                           "buttonId": 1, "numButtonsInGroup": 2, "handleName": p(param), "gestureBehaviour": "Toggle"},
                          rect(960, 190 + i * 72, 300, 60)))
    kids.append(entry("s_hint1", "Label", label_text("Your WAVs go in a folder called", 20), rect(940, 410, 330, 28)))
    kids.append(entry("s_hint2", "Label", label_text("'Twisted Block Samples'", 20), rect(940, 438, 330, 28)))
    kids.append(entry("s_hint3", "Label", label_text("at the top of the same drive.", 20), rect(940, 466, 330, 28)))
    slots(kids, [[(P_INST, "knob"), (P_FILE, "file"), (P_START, "knob"), (P_END, "knob")],
                 [(P_LOOP, "menu"), (P_LSTART, "knob"), (P_GAIN, "knob"), (P_REVERSE, "menu")],
                 [(P_SLMODE, "menu"), (P_SLICES, "knob"), (P_SENSE, "knob"), (P_OCTAVE, "knob")],
                 [(P_MODE, "menu"), (P_LANE, "menu"), (P_STEP, "knob"), (P_VOLUME, "knob")]],
          20, 296, 222, 78, gapx=6)
    return component(kids, background="ffffffff")


def ctrl_slot_sized(overlay, w, h, value_px):
    c = ctrl_slot(overlay, w, h)
    c["componentsData"][2]["componentData"]["data"]["textStyle"] = text_style(value_px)
    return c


def build(g):
    tabs = [("GRID", grid_tab(g)), ("SETUP", setup_tab(g)), ("SAMPLE", sample_tab(g))]
    defs = []
    for w, h in ((146, 112), (220, 112), (222, 78)):
        defs.append({"key": "slot_knob_%d" % w, "value": ctrl_slot_sized("knob overlay", w, h, 34 if h > 80 else 30)})
        defs.append({"key": "slot_menu_%d" % w, "value": ctrl_slot_sized("menu overlay", w, h, 34 if h > 80 else 30)})
    defs.append({"key": "slot_file_222", "value": ctrl_slot_sized("knob overlay", 222, 78, 22)})
    defs += [{"key": "knob overlay", "value": knob_overlay()}, {"key": "menu overlay", "value": menu_overlay()}]
    tui = {"pageData": {
        "version": 1,
        "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": defs},
        "replacementStrings": [
            {"key": p(P_LANE), "value": ["1", "2", "3", "4"]},
            {"key": p(P_FIELD), "value": ["Note", "Inst", "Vol", "Cmd", "Val"]},
            {"key": p(P_FOLLOW), "value": ["Off", "On"]},
            {"key": p(P_STRESS), "value": ["Off", "On"]},
            {"key": p(P_REDRAW), "value": ["Notify", "Rebuild", "Both", "Poll"]},
            {"key": p(P_MODE), "value": ["Play", "Edit"]},
            {"key": p(P_TRIGGER), "value": ["Gate", "One-shot"]},
            {"key": p(P_QUANT), "value": ["Off", "Beat", "Bar"]},
            {"key": p(P_LOOP), "value": ["Off", "Forward", "PingPong"]},
            {"key": p(P_REVERSE), "value": ["Off", "On"]},
            {"key": p(P_SLMODE), "value": ["Off", "Grid", "Transient"]},
        ],
        "info": {"version": 1, "type": "CompleteDescription"},
        "tabs": [{"version": 1, "tabName": name, "fnKeyIndex": i, "fnKeySubIndex": 0,
                  "componentDefinition": comp, "qlinkBoundsData": []} for i, (name, comp) in enumerate(tabs)],
    }}
    n = sum(len(t[1]["componentsData"]) for t in tabs)
    return tui, n


def qlinks():
    def bank(cols):
        m = {"Q-Link %d" % i: -1 for i in range(1, 17)}
        for b, column in enumerate(cols):          # column 0 -> Q-Links 13-16, 1 -> 9-12, 2 -> 5-8, 3 -> 1-4
            for k, param in enumerate(column):
                m["Q-Link %d" % (13 - 4 * b + k)] = param
        return m
    grid = bank([[P_CURSOR, P_FIELD, P_VALUE, P_LANE], [P_MODE, P_INST, P_OCTAVE, P_STEP],
                 [P_BLOCK, P_LENGTH, P_LPB, P_FOLLOW], [P_VOLUME, P_TICKS, P_TRIGGER, P_QUANT]])
    setup = bank([[P_BASE, P_TRIGGER, P_QUANT, P_TICKS], [P_VOLUME, P_REDRAW, P_LPB, P_FOLLOW]])
    sample = bank([[P_INST, P_FILE, P_START, P_END], [P_LOOP, P_LSTART, P_GAIN, P_REVERSE],
                   [P_SLMODE, P_SLICES, P_SENSE, P_OCTAVE], [P_MODE, P_LANE, P_STEP, P_VOLUME]])
    tabs = [grid, setup, sample]
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": [
                {"Tab": i + 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": m} for i, m in enumerate(tabs)]},
            "Program Mode Q-Links": dict(grid)}


def dump(obj):
    # The overlay widgets need a duplicate "version" key; json can't hold one, so patch the text.
    return json.dumps(obj, indent=1).replace('"__DUPV__"', '"version"') + "\n"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(root, "plugin", "skins"))
    ap.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf")
    a = ap.parse_args()
    g = Gen(a.out, a.font)
    tui, n = build(g)
    with open(os.path.join(a.out, "TUI.json"), "w") as f:
        f.write(dump(tui))
    with open(os.path.join(a.out, "Q-Links.json"), "w") as f:
        f.write(dump(qlinks()))
    print("skin written to %s (%d components, %d plugin parameters expected)" % (a.out, n, NUM_PARAMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
