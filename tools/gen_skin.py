#!/usr/bin/env python3
"""Generates the hand-built MPC skin for Twisted Block into plugin/skins/.

Writes:
  plugin/skins/TUI.json      the touch screen (one GRID tab)
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
 P_MODE, P_INST, P_OCTAVE, P_STEP) = range(16)
P_ROW_BASE = 16
P_CURSOR_SLOT = P_ROW_BASE + ROWS * COLS
P_PLAY_SLOT = P_CURSOR_SLOT + 1
P_ACT_BASE = P_PLAY_SLOT + 1
P_WAVE_BASE = P_ACT_BASE + LANES
NUM_PARAMS = P_WAVE_BASE + WAVE_BARS
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


def build(g):
    kids = []
    # --- header: column titles + field underline (which field the cursor edits)
    for c, (x, w) in enumerate(COL_BOX):
        kids.append(entry("title%d" % c, "Label", label_text(COL_TITLES[c], 22, "horizontallyCentred verticallyCentred"),
                          rect(x - 6, 0, w + 12, HEADER_H - 8)))
    for f in range(FIELDS):
        x, w = COL_BOX[f + 1]
        kids.append(entry("fieldmark%d" % f, "Indicator",
                          {"version": 1, "onImage": g.field_underline(w + 8), "offImage": g.clear(w + 8, 6),
                           "indicatorId": f, "numIndicatorsInGroup": FIELDS, "handleName": p(P_FIELD)},
                          rect(x - 4, HEADER_H - 8, w + 8, 6)))

    # --- row backgrounds: cursor bar, centre lines, playhead marker
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

    # --- glyph cells
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
    # playhead marker on top of the text
    for r in range(ROWS):
        y = ROW_Y0 + r * ROW_H
        kids.append(entry("play%02d" % r, "Indicator",
                          {"version": 1, "onImage": play_on, "offImage": clear_row, "indicatorId": r,
                           "numIndicatorsInGroup": ROWS + 1, "handleName": p(P_PLAY_SLOT)},
                          rect(0, y, GRID_W, ROW_H)))
    # touch targets: one radio button per editable cell
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

    # --- right panel: lane buttons + activity lamps
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

    # --- waveform: 64 meters
    wx, wy, bw, wh = PANEL_X, 96, 7, 120
    bar_off = g.solid("bar_off.png", bw, wh, (238, 238, 238, 255))
    bar_on = g.save(Image.new("RGBA", (bw, wh), CLEAR), "bar_tmp.png")
    im = Image.new("RGBA", (bw, wh), CLEAR)
    ImageDraw.Draw(im).rectangle([0, 0, bw - 2, wh - 1], fill=BLACK)
    bar_on = g.save(im, "bar_on.png")
    os.remove(os.path.join(g.img, "bar_tmp.png"))
    for b in range(WAVE_BARS):
        kids.append(entry("wave%02d" % b, "Meter",
                          {"version": 3, "direction": "Up", "invert": False, "inactiveImage": bar_off,
                           "peakImage": bar_on, "rmsImage": bar_on, "peakHandle": p(P_WAVE_BASE + b),
                           "rmsHandle": p(P_WAVE_BASE + b), "peakHoldHandle": p(P_WAVE_BASE + b),
                           "peakHoldSize": 1, "peakHoldColour": "ff000000"},
                          rect(wx + b * bw, wy, bw, wh)))
    kids.append(entry("waveframe", "Image", {"version": 2, "imageType": "Regular", "colour": "0",
                                             "image": g.wave_frame(WAVE_BARS * bw + 8, wh + 8)},
                      rect(wx - 4, wy - 4, WAVE_BARS * bw + 8, wh + 8)))

    # parameter count readout
    kids.append(entry("pcount_name", "Label", label_text("PARAMS", 22), rect(PANEL_X, 226, 110, 34)))
    kids.append(entry("pcount", "Label", {"version": 1, "textStyle": text_style(30), "type": "Value", "handleName": p(P_PCOUNT)},
                      rect(PANEL_X + 110, 226, 120, 34)))

    # stress test toggle
    def stress_img(on):
        w_, h_ = 170, 40
        im = Image.new("RGBA", (w_, h_), (190, 190, 190, 255) if on else WHITE)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, w_ - 1, h_ - 1], outline=BLACK if on else (140, 140, 140, 255), width=4 if on else 2)
        f = ImageFont.truetype(FONT_FILE, 24)
        t = "STRESS " + ("ON" if on else "OFF")
        l, tt, r, b = d.textbbox((0, 0), t, font=f)
        d.text(((w_ - (r - l)) / 2 - l, (h_ - (b - tt)) / 2 - tt), t, font=f, fill=BLACK)
        return g.save(im, "stress_%s.png" % ("on" if on else "off"))
    kids.append(entry("stress", "Button", {"version": 2, "onImage": stress_img(True), "offImage": stress_img(False),
                                           "buttonId": 1, "numButtonsInGroup": 2, "handleName": p(P_STRESS),
                                           "gestureBehaviour": "Toggle"},
                      rect(PANEL_X + 250, 224, 170, 40)))

    # --- edit controls, 3 columns x 4 rows (column = one Q-Link bank)
    slots = [(P_CURSOR, "knob"), (P_FIELD, "menu"), (P_VALUE, "knob"), (P_LANE, "menu"),
             (P_MODE, "menu"), (P_INST, "knob"), (P_OCTAVE, "knob"), (P_STEP, "knob"),
             (P_LENGTH, "knob"), (P_LPB, "knob"), (P_FOLLOW, "menu"), (P_REDRAW, "menu")]
    sw, sh = 146, 86
    for i, (param, kind) in enumerate(slots):
        col, row = divmod(i, 4)
        kids.append(entry("ctrl%d" % param, "slot_" + kind, {"version": 1, "handleName": "Data"},
                          rect(PANEL_X + col * (sw + 6), 268 + row * (sh + 4), sw, sh),
                          remap=[{"key": "Data", "value": p(param)}], focus="Yes", show="Hide"))

    root = component(kids, background="ffffffff")
    tui = {"pageData": {
        "version": 1,
        "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": [
            {"key": "slot_knob", "value": ctrl_slot("knob overlay", sw, sh)},
            {"key": "slot_menu", "value": ctrl_slot("menu overlay", sw, sh)},
            {"key": "knob overlay", "value": knob_overlay()},
            {"key": "menu overlay", "value": menu_overlay()},
        ]},
        "replacementStrings": [
            {"key": p(P_LANE), "value": ["1", "2", "3", "4"]},
            {"key": p(P_FIELD), "value": ["Note", "Inst", "Vol", "Cmd", "Val"]},
            {"key": p(P_FOLLOW), "value": ["Off", "On"]},
            {"key": p(P_STRESS), "value": ["Off", "On"]},
            {"key": p(P_REDRAW), "value": ["Notify", "Rebuild", "Both", "Poll"]},
            {"key": p(P_MODE), "value": ["Play", "Edit"]},
        ],
        "info": {"version": 1, "type": "CompleteDescription"},
        "tabs": [{"version": 1, "tabName": "GRID", "fnKeyIndex": 0, "fnKeySubIndex": 0,
                  "componentDefinition": root, "qlinkBoundsData": []}],
    }}
    return tui, len(kids)


def qlinks():
    m = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    # Top row of knobs: the editing set. Next row: block settings.
    m.update({"Q-Link 13": P_CURSOR, "Q-Link 14": P_FIELD, "Q-Link 15": P_VALUE, "Q-Link 16": P_LANE,
              "Q-Link 9": P_MODE, "Q-Link 10": P_INST, "Q-Link 11": P_OCTAVE, "Q-Link 12": P_STEP,
              "Q-Link 5": P_LENGTH, "Q-Link 6": P_LPB, "Q-Link 7": P_FOLLOW, "Q-Link 8": P_REDRAW,
              "Q-Link 1": P_VOLUME, "Q-Link 2": P_STRESS})
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": [
                {"Tab": 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": m}]},
            "Program Mode Q-Links": dict(m)}


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
