#!/usr/bin/env python3
"""Build data/mister_kun.otf from data/mister_kun.svg.

The MiSTer tab in the settings draws its icon the same way every other tab
does: as a glyph in a font merged into the ImGui atlas. That keeps it aligned
with the label beside it and sharp at every display scale, neither of which a
bitmap would manage. This turns the mascot artwork into that glyph.

The artwork is two-tone -- black and white shapes layered over one another --
while a glyph has only one colour, so the shapes are composited in the order
they are painted: black adds, white subtracts. That ordering matters, because
the mark draws black over white in several places, and flattening it by simply
subtracting every white shape at the end would erase those details. Real
boolean operations are used rather than a winding-order trick, since the source
is an Illustrator export whose subpath directions cannot be relied on.

Composited faithfully the mark is hollow line art, which at the size a tab icon
is drawn reads far lighter than the solid Font Awesome glyphs beside it. One
white shape fills the body and is the whole reason it is hollow, so by default
that shape alone is dropped, filling the mark in while every smaller detail
still knocks out of it. Pass --outline to keep the artwork's own hollow form.

Requires fonttools and skia-pathops. Neither is needed to build xemu; run this
only when the artwork changes, and commit the result.

    pip install fonttools skia-pathops
    scripts/gen-mister-kun-font.py
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import pathops
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.svgLib.path import parse_path
except ImportError as exc:  # pragma: no cover - depends on the environment
    sys.exit(f"{exc}. Install with: pip install fonttools skia-pathops")

SVG_NS = "{http://www.w3.org/2000/svg}"

# Just above the Font Awesome range, alongside the button glyphs in abxy.ttf.
CODEPOINT = 0xF904

UNITS_PER_EM = 1000

# The glyph is drawn to sit on the baseline and rise to about cap height, so it
# lines up with the text beside it without the font manager having to nudge it.
INK_HEIGHT = 750
INK_BASELINE = 0

# Room either side, so the icon does not touch the label.
SIDE_BEARING = 60


def parse_length(value):
    """Read an SVG coordinate, ignoring any unit suffix."""
    match = re.match(r"^\s*(-?[\d.]+)", value or "")
    if not match:
        raise ValueError(f"cannot read length {value!r}")
    return float(match.group(1))


def shape_to_path_data(element):
    """Return path data for the shapes this artwork uses, or None for others."""
    tag = element.tag.replace(SVG_NS, "")

    if tag == "path":
        return element.get("d")

    if tag == "polygon":
        points = element.get("points", "").strip()
        if not points:
            return None
        return f"M {points} Z"

    if tag == "rect":
        x = parse_length(element.get("x", "0"))
        y = parse_length(element.get("y", "0"))
        w = parse_length(element.get("width", "0"))
        h = parse_length(element.get("height", "0"))
        if w <= 0 or h <= 0:
            return None
        return f"M {x},{y} H {x + w} V {y + h} H {x} Z"

    return None


def is_cutout(fill):
    """White shapes are holes in the black ones, not shapes of their own."""
    return (fill or "").strip().lower() in ("#ffffff", "#fff", "white")


def collect(element, view_box, inherited_fill, shapes):
    """Walk the tree, gathering drawable shapes in the order they are painted."""
    min_x, min_y, max_x, max_y = view_box

    for child in element:
        tag = child.tag.replace(SVG_NS, "")

        # A transform would silently move artwork if it were ignored.
        if child.get("transform"):
            raise ValueError(f"<{tag}> carries a transform, which is not handled")

        fill = child.get("fill", inherited_fill)

        if tag == "g":
            collect(child, view_box, fill, shapes)
            continue

        data = shape_to_path_data(child)
        if not data:
            continue

        path = pathops.Path()
        parse_path(data, path.getPen())

        # The export carries colour swatches outside the canvas. Keeping them
        # would leave the glyph mostly empty space.
        bounds = path.bounds
        if bounds is None:
            continue
        if (bounds[2] <= min_x or bounds[0] >= max_x or
                bounds[3] <= min_y or bounds[1] >= max_y):
            continue

        shapes.append((path, is_cutout(fill)))


def bbox_area(path):
    bounds = path.bounds
    if bounds is None:
        return 0.0
    return (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])


def build_outline(svg_path, solid_body=False):
    root = ET.parse(svg_path).getroot()

    view_box = root.get("viewBox")
    if not view_box:
        raise ValueError("the SVG has no viewBox")
    vb_x, vb_y, vb_w, vb_h = (float(v) for v in view_box.replace(",", " ").split())

    shapes = []
    collect(root, (vb_x, vb_y, vb_x + vb_w, vb_y + vb_h), root.get("fill"), shapes)

    if not any(not cut for _, cut in shapes):
        raise ValueError("no filled shapes found")

    # The artwork layers black over white and white over black in places, so
    # the shapes are composited in the order they are painted. Subtracting
    # every white shape at the end instead would erase the details drawn on
    # top of them, which costs the pupils, the nose and the toes.
    skip = None
    if solid_body:
        # One white shape fills the body and is what makes the mark an outline
        # rather than a silhouette. Dropping it, and only it, fills the mark in
        # while leaving every smaller detail to knock out of it as usual.
        whites = [path for path, cut in shapes if cut]
        skip = max(whites, key=bbox_area) if whites else None

    result = pathops.Path()
    for path, cut in shapes:
        if path is skip:
            continue
        combined = pathops.Path()
        if cut:
            pathops.difference([result], [path], combined.getPen())
        else:
            pathops.union([result, path], combined.getPen())
        result = combined

    # Resolves the self-intersections compositing leaves behind, which a
    # charstring cannot express.
    return pathops.simplify(result)


def place(path):
    """Scale and flip the artwork into font units, centred on its own ink."""
    bounds = path.bounds
    if bounds is None:
        raise ValueError("the combined outline is empty")

    min_x, min_y, max_x, max_y = bounds
    height = max_y - min_y
    if height <= 0:
        raise ValueError("the combined outline has no height")

    # SVG counts y downwards and fonts count it upwards, so the vertical scale
    # is negative and the origin moves to the top of the ink.
    scale = INK_HEIGHT / height
    width = (max_x - min_x) * scale
    advance = round(width + 2 * SIDE_BEARING)

    transform = (
        scale, 0,
        0, -scale,
        SIDE_BEARING - min_x * scale,
        INK_BASELINE + max_y * scale,
    )

    return path.transform(*transform), advance


def build_font(outline, advance, out_path):
    pen = T2CharStringPen(advance, glyphSet=None)
    outline.draw(pen)

    glyph_name = "misterkun"
    builder = FontBuilder(UNITS_PER_EM, isTTF=False)
    builder.setupGlyphOrder([".notdef", glyph_name])
    builder.setupCharacterMap({CODEPOINT: glyph_name})

    empty = T2CharStringPen(advance, glyphSet=None)
    builder.setupCFF(
        "MiSTerKun",
        {"FullName": "MiSTer Kun", "Weight": "Regular"},
        {".notdef": empty.getCharString(), glyph_name: pen.getCharString()},
        {},
    )

    builder.setupHorizontalMetrics({
        ".notdef": (advance, 0),
        glyph_name: (advance, SIDE_BEARING),
    })
    builder.setupHorizontalHeader(ascent=INK_HEIGHT, descent=0)
    builder.setupNameTable({
        "familyName": "MiSTer Kun",
        "styleName": "Regular",
        "psName": "MiSTerKun-Regular",
        # Informal terms, from the artwork's own licence: a gift to the MiSTer
        # community, to be used and remixed as people wish, with accreditation
        # appreciated where possible.
        "designer": "baxysquare",
        "licenseDescription": "Provided by its author for free use and "
                              "modification by the MiSTer community.",
    })
    builder.setupOS2(sTypoAscender=INK_HEIGHT, sTypoDescender=0,
                     usWinAscent=INK_HEIGHT, usWinDescent=0)
    builder.setupPost()

    builder.save(out_path)


def main():
    repo_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path,
                        default=repo_root / "data" / "mister_kun.svg")
    parser.add_argument("--output", type=Path,
                        default=repo_root / "data" / "mister_kun.otf")
    parser.add_argument("--outline", action="store_true",
                        help="keep the artwork's hollow line-art form instead "
                             "of filling the body in")
    args = parser.parse_args()

    outline = build_outline(args.input, solid_body=not args.outline)
    placed, advance = place(outline)
    build_font(placed, advance, args.output)

    try:
        shown = args.output.relative_to(repo_root)
    except ValueError:
        shown = args.output
    print(f"{shown}: {args.output.stat().st_size} bytes, "
          f"U+{CODEPOINT:04X}, advance {advance}/{UNITS_PER_EM}")


if __name__ == "__main__":
    main()
