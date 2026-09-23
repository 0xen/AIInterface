"""Ring gauges and date-timeline bars.

A ring is drawn with `ui.progress_ring` -- a real anti-aliased arc -- when the
running app has it (`native(ui)`), and otherwise falls back to the old
construction below, so a script using `ring()` works on either build. The
native ring keeps about the same footprint as the fallback, so a layout
tuned for one looks right on the other. `native(ui)` is also the test for
whether `ui.set_tooltip` only shows on hover: the same build fixed both.

The fallback: older builds of `aii.ui` had no draw-list (no circles, no
lines), so a ring there is a circle of 〇 glyphs placed with `same_line(offset)` on consecutive text rows, filled
clockwise from 12 o'clock in as many colours as you give it. The glyph is
U+3007, which is inside the Japanese range the app's font atlas loads; a
Unicode dot like U+25CF is not, and would draw as a missing-glyph box -- so
do not "improve" the glyph without checking that range first (the atlas is
Latin-1 plus ImGui's Japanese ranges, nothing else: no arrows, no em dash).

Geometry assumes the app's style: a 15 px font and 5 px item spacing, so a
text row advances 20 px and a full-width glyph is 15 px wide. Every function
takes the `ui` module first, like `widgets.py`.

    from aii_ui import gauges
    ui.begin_group()
    gauges.ring(ui, [(0.3, gauges.shade(BLUE, 0.6)), (0.2, BLUE)],
                ["Build", "14/20", "70%"])
    ui.end_group()

Also `timeline(ui, spans, width)`: rows of coloured bars placed along a date
axis, for a Gantt-like "new content until X, then practice exams" strip.
"""

import math

ROW_H = 20.0      # text line + item spacing, at the app's 15 px font
GLYPH_W = 15.0    # a full-width glyph at 15 px
GLYPH = "〇"  # 〇
EMPTY = (0.32, 0.32, 0.36, 1.0)

_cache = {}


def native(ui):
    """True when the running app draws rings itself (`ui.progress_ring`),
    which is also the build where `ui.set_tooltip` shows only on hover."""
    return hasattr(ui, "progress_ring")


def text_width(s):
    """A rough width in pixels for `s` at the app's 15 px font, for centring
    text: there is no calc_text_size in `aii.ui`. Close enough to centre a
    number; do not use it to butt two items together."""
    w = 0.0
    for ch in s:
        o = ord(ch)
        if o >= 0x2E80:
            w += 15.0
        elif ch in "il.,:;|!'":
            w += 3.5
        elif ch in " /()[]-":
            w += 5.0
        elif ch in "%MWmw":
            w += 11.0
        elif ch.isupper():
            w += 9.0
        else:
            w += 7.5
    return w


def shade(rgba, k):
    """`rgba` scaled toward black (k < 1) or white (k > 1), alpha kept."""
    r, g, b = rgba[0], rgba[1], rgba[2]
    a = rgba[3] if len(rgba) > 3 else 1.0
    if k <= 1.0:
        return (r * k, g * k, b * k, a)
    t = min(1.0, k - 1.0)
    return (r + (1 - r) * t, g + (1 - g) * t, b + (1 - b) * t, a)


def _layout(rows):
    """Ring dot positions for a ring `2*rows+1` text rows tall: a list of
    (row index from the top, x of the dot's left edge in px, clockwise angle
    0..1 from 12 o'clock). Each text row covers a band of the circle; the dots
    in a band are spread across it and mirrored left/right so the ring is
    symmetric."""
    if rows in _cache:
        return _cache[rows]
    rx = rows * ROW_H                    # radius, px
    gap = GLYPH_W * 1.2
    dots = []
    for r in range(-rows, rows + 1):
        a = abs(r)
        ylo = max(0.0, (a - 0.5) * ROW_H)
        yhi = min(rx, (a + 0.5) * ROW_H)
        xo = math.sqrt(max(0.0, rx * rx - ylo * ylo))   # outer extent of the band
        xi = math.sqrt(max(0.0, rx * rx - yhi * yhi))   # inner extent
        if a == rows:
            xs = [0.0]
            k = 1
            while k * gap <= xo + 1e-6:
                xs += [-k * gap, k * gap]
                k += 1
        else:
            span = xo - xi
            n = int(span // gap) + 1
            if n == 1:
                half = [math.sqrt(max(0.0, rx * rx - (a * ROW_H) ** 2))]
            else:
                half = [xo - (j + 0.5) * span / n for j in range(n)]
            xs = [-h for h in half] + half
        for x in xs:
            t = (math.atan2(x, -r * ROW_H) / (2.0 * math.pi)) % 1.0
            dots.append((r + rows, rx + x, t))
    _cache[rows] = dots
    return dots


def ring(ui, parts, center_lines=(), rows=4, empty=EMPTY, center_colors=None,
         caption="", thickness=10.0):
    """Draw a ring gauge at the cursor, about `2*rows*20+15` px across.
    Call inside `begin_group()`/`end_group()` so several rings can sit side
    by side with `same_line()` between groups (the fallback needs the group
    for its offsets; the native ring does not mind it).

    `parts` is [(fraction, rgba), ...], drawn clockwise from 12 o'clock in
    order; the remainder is `empty`. `center_lines` (up to rows*2-1 strings)
    are centred inside the ring; `center_colors` optionally colours each one.
    `caption` is a line beneath the ring, in the disabled colour.
    `thickness` is the native ring's stroke width in px.

    Returns True when the ring was drawn natively, in which case the ring is
    the last item and a `ui.set_tooltip()` straight after covers it."""
    lines = list(center_lines)[: max(1, rows * 2 - 1)]
    if native(ui):
        cols = None
        if center_colors:
            cols = [center_colors[i] if i < len(center_colors) else None
                    for i in range(len(lines))]
        ui.progress_ring([(max(0.0, f), c) for f, c in parts] or 0.0,
                         radius=ring_width(rows) / 2.0, thickness=thickness,
                         track=empty, label="\n".join(lines), caption=caption,
                         label_colors=cols)
        return True
    _glyph_ring(ui, parts, lines, rows, empty, center_colors)
    if caption:
        at(ui, ring_width(rows) / 2.0 - text_width(caption) / 2.0)
        ui.text_disabled(caption)
    return False


def _glyph_ring(ui, parts, lines, rows, empty, center_colors):
    """The fallback ring, `2*rows+1` text rows of 〇 glyphs."""
    dots = _layout(rows)
    n = len(dots)
    order = sorted(range(n), key=lambda i: dots[i][2])
    colour_of = [empty] * n
    acc = 0.0
    k = 0
    for frac, rgba in parts:
        acc += max(0.0, frac)
        upto = int(round(min(1.0, acc) * n))
        while k < upto:
            colour_of[order[k]] = rgba
            k += 1
    # A sliver of progress should still show one dot.
    if k == 0 and parts and sum(max(0.0, f) for f, _ in parts) > 0:
        colour_of[order[0]] = parts[0][1]

    rx = rows * ROW_H
    first = rows - (len(lines) - 1) // 2 if lines else None
    by_row = {}
    for i, (r, x, _t) in enumerate(dots):
        by_row.setdefault(r, []).append((x, GLYPH, colour_of[i]))
    for li, s in enumerate(lines):
        col = center_colors[li] if center_colors and li < len(center_colors) else None
        by_row.setdefault(first + li, []).append(
            (rx + GLYPH_W / 2.0 - text_width(s) / 2.0, s, col))
    for r in range(2 * rows + 1):
        ui.text(" ")
        for x, s, col in sorted(by_row.get(r, []), key=lambda e: e[0]):
            ui.same_line(max(0.01, x))
            if col is None:
                ui.text(s)
            else:
                ui.text_colored(col, s)


def ring_width(rows=4):
    return 2 * rows * ROW_H + GLYPH_W


def at(ui, x):
    """Start a new row and move the cursor to `x` px from the group's left
    edge, so the next item lands there."""
    ui.text(" ")
    ui.same_line(max(0.01, x))


def timeline(ui, start, end, spans, width, label_w=0.0, bar_h=16.0):
    """Rows of date bars along one axis. `start`/`end` are datetime.date;
    `spans` is [(label, from_date, to_date, rgba, caption), ...], one row
    each, clipped to the axis; `caption` is drawn inside the bar. The axis
    runs from `label_w` px to `label_w + width` px, so it lines up with a
    plot of the same width placed at `label_w`. Uses progress bars as solid
    colour bands (COL_PLOT_HISTOGRAM)."""
    total = max(1, (end - start).days)
    for label, a, b, rgba, caption in spans:
        ui.text(label)
        if a is None or b is None:
            continue
        a = max(start, min(end, a))
        b = max(start, min(end, b))
        x0 = label_w + width * (a - start).days / total
        x1 = label_w + width * (b - start).days / total
        ui.same_line(max(0.01, x0))
        ui.push_style_color(ui.COL_PLOT_HISTOGRAM, rgba)
        ui.progress_bar(1.0, max(4.0, x1 - x0), bar_h, caption or " ")
        ui.pop_style_color()


def x_of(start, end, day, width, label_w=0.0):
    """Pixel x of `day` on a `timeline`/plot axis."""
    total = max(1, (end - start).days)
    return label_w + width * (max(start, min(end, day)) - start).days / total
