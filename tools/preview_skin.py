#!/usr/bin/env python3
"""Draws a mock-up of the MPC screen from plugin/skins/TUI.json and a parameter dump.

    python3 tools/preview_skin.py <dump.json> <out.png> [tab number, 0 = first]

The dump comes from tools/test_grid.cpp (--dump). This is an approximation for
checking layout on a computer: the MPC uses its own fonts (Roboto here is
replaced by DejaVu Sans) and renderer.
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIN = os.path.join(ROOT, "plugin", "skins")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def load_tui():
    # The duplicate "version" keys are harmless for this reader (last one wins).
    with open(os.path.join(SKIN, "TUI.json")) as f:
        return json.load(f)["pageData"]


def main(dump_path, out_path, tab_index=0):
    page = load_tui()
    params = {p["index"]: p for p in json.load(open(dump_path))["params"]}
    comps = {c["key"]: c["value"] for c in page["componentDefinitions"]["localComponentDefinitions"]}
    images = {}

    def img(path):
        if path not in images:
            images[path] = Image.open(os.path.join(SKIN, path)).convert("RGBA")
        return images[path]

    def resolve(handle, remaps):
        for remap in remaps:
            for m in remap:
                if m["key"] == handle:
                    handle = m["value"]
                    break
        return handle

    def pnorm(handle):
        if handle.startswith("Parameter "):
            return params[int(handle.split()[1])]["norm"]
        return 0.0

    def ptext(handle, field):
        if handle.startswith("Parameter "):
            return params[int(handle.split()[1])][field]
        return ""

    screen = Image.new("RGBA", (1280, 800), (255, 255, 255, 255))
    d0 = ImageDraw.Draw(screen)
    d0.rectangle([0, 0, 1279, 109], fill=(225, 225, 225, 255))
    d0.text((20, 35), "MPC  |  Track 1  |  Twisted Block  (mock-up)", font=ImageFont.truetype(FONT, 30), fill=(0, 0, 0, 255))
    d0.rectangle([0, 738, 1279, 799], fill=(225, 225, 225, 255))
    for i, t in enumerate(page["tabs"]):
        f = ImageFont.truetype(FONT, 30)
        if i == tab_index:
            d0.rectangle([20 + i * 220, 742, 220 + i * 220, 796], outline=(0, 0, 0, 255), width=4)
        d0.text((40 + i * 220, 750), t["tabName"], font=f, fill=(0, 0, 0, 255))
    tab = Image.new("RGBA", (1280, 628), (255, 255, 255, 255))

    def draw(component, ox, oy, remaps):
        colour = component["backgroundData"]["unfocussed"]["colour"]
        for e in component["componentsData"]:
            if e["bounds"].get("whenVisible") == "WhenFocussed":
                continue
            x, y, w, h = [float(v) for v in e["bounds"]["bounds"].split()]
            x += ox
            y += oy
            cd = e["componentData"]
            t, data = cd["type"], cd["data"]
            rm = [e["handle remapping"]["map"]] + remaps
            if t in comps:
                draw(comps[t], x, y, rm)
                continue
            handle = resolve(data.get("handleName", "Invalid"), rm) if "handleName" in data else "Invalid"
            if t == "Image":
                im = img(data["image"])
                if data.get("imageType") == "FilmStrip":
                    n = data["numFrames"]
                    fh = im.height // n
                    frame = round(pnorm(handle) * (n - 1))
                    im = im.crop((0, frame * fh, im.width, (frame + 1) * fh))
                tab.alpha_composite(im.resize((int(w), int(h))), (int(x), int(y)))
            elif t in ("Indicator", "Button"):
                n = max(data.get("numIndicatorsInGroup", data.get("numButtonsInGroup", 1)), 2)
                step = round(pnorm(handle) * (n - 1))
                ident = data.get("indicatorId", data.get("buttonId", 1))
                im = img(data["onImage"] if step == ident else data["offImage"])
                tab.alpha_composite(im, (int(x + (w - im.width) / 2), int(y + (h - im.height) / 2)))
            elif t == "Meter":
                v = pnorm(resolve(data["peakHandle"], rm))
                off = img(data["inactiveImage"]).resize((int(w), int(h)))
                on = img(data["peakImage"]).resize((int(w), int(h)))
                tab.alpha_composite(off, (int(x), int(y)))
                cut = int(h * (1 - v))
                if cut < h:
                    tab.alpha_composite(on.crop((0, cut, int(w), int(h))), (int(x), int(y) + cut))
            elif t == "Label":
                ts = data["textStyle"]
                font = ImageFont.truetype(FONT, int(ts["font"]["height"] * 0.85))
                if data["type"] == "Text":
                    s = data["text"]
                elif data["type"] == "Name":
                    s = ptext(handle, "name")
                else:
                    s = ptext(handle, "text")
                col = ts["colour"]
                rgba = (int(col[2:4], 16), int(col[4:6], 16), int(col[6:8], 16), int(col[0:2], 16))
                d = ImageDraw.Draw(tab)
                l, tt, r, b = d.textbbox((0, 0), s, font=font)
                tx = x if "left" in ts["justification"] else x + (w - (r - l)) / 2
                d.text((tx - l, y + (h - (b - tt)) / 2 - tt), s, font=font, fill=rgba)

    root = page["tabs"][tab_index]["componentDefinition"]
    draw(root, 0, 0, [])
    screen.alpha_composite(tab, (0, 110))
    screen.convert("RGB").save(out_path)
    print("wrote", out_path)


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) == 4 else 0)
