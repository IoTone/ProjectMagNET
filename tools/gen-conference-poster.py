#!/usr/bin/env python3
"""
gen-conference-poster.py — generate a conference poster PPTX scaffold.

Produces a single-slide A0-portrait (841 × 1189 mm) Microsoft PowerPoint
file matching the project's standard navy + light-blue academic-poster
template (`Title Here / Introduction / Methods / Results / ...`).

Layout (top-to-bottom, left-to-right):

    Title band       95 mm   navy on white, "Title Here" 100 pt
    Author strip     50 mm   "Author One¹, Author Two², ..." + email
    3-column grid    260 mm  ×  18 mm gutters  ×  12.5 mm side margins

Sections:
    Left:     Introduction, Objectives
    Center:   Methods, Figure 1, Figure 2, Figure 3
    Right:    Results, Conclusions, Future Work, Acknowledgements,
              References + Contact (side-by-side; Contact has a QR slot)

All section text is placeholder copy ("Your text goes here…"). Open the
output in PowerPoint and edit; the figure rectangles can be deleted +
replaced via Insert → Picture.

Requirements:
    python3 + python-pptx  (pip install python-pptx)

Usage:
    python3 tools/gen-conference-poster.py
        # writes ./conference-poster.pptx

    python3 tools/gen-conference-poster.py -o ~/Desktop/mag-net-poster.pptx
        # writes to a custom path

The script's layout/style helpers are factored so re-running with edited
placeholder text is the typical workflow. Each section is one
`add_section(...)` call near the bottom of the file — find the heading,
edit the bullets/figure data, re-run.

See `MagNET_M5DialFiddlerCrab/README.md` and the project root for usage
context; see the print-guidance footer baked into the slide for the
typical export settings (PDF/X-1a, 300 dpi, CMYK if required, 3–5 mm
bleed, 2 mm safety margin).
"""

import argparse
import os
import sys
from pptx import Presentation
from pptx.util import Mm, Pt
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.oxml.ns import qn
from lxml import etree

# --- Palette (matches the navy/light-blue template image) ---
NAVY_DARK    = RGBColor(0x25, 0x41, 0x6A)   # title band
NAVY_MID     = RGBColor(0x3A, 0x5A, 0x8A)   # section header bars
LIGHT_BLUE   = RGBColor(0xE5, 0xEE, 0xF7)   # section body fill
BORDER_BLUE  = RGBColor(0x6B, 0x8B, 0xB3)   # thin border around content boxes
WHITE        = RGBColor(0xFF, 0xFF, 0xFF)
BODY_TEXT    = RGBColor(0x1F, 0x2A, 0x44)   # dark navy for body on light blue
MUTED_TEXT   = RGBColor(0x55, 0x66, 0x88)
PLACEHOLDER  = RGBColor(0xC2, 0xCF, 0xDE)   # figure-placeholder fill

# --- Page geometry (A0 portrait) ---
PAGE_W_MM       = 841
PAGE_H_MM       = 1189
MARGIN_SIDE_MM  = 12.5
TITLE_BAND_H    = 95
AUTHOR_BAND_H   = 50
COL_W           = 260
GUTTER          = 18
SEC_HEADER_H    = 36
SEC_GAP         = 16


# ────────────────────────────────────────────────────────────────────────
# low-level helpers
# ────────────────────────────────────────────────────────────────────────

def add_rect(slide, x_mm, y_mm, w_mm, h_mm,
             fill_rgb, line_rgb=None, line_w_pt=0.75):
    shp = slide.shapes.add_shape(
        MSO_SHAPE.RECTANGLE, Mm(x_mm), Mm(y_mm), Mm(w_mm), Mm(h_mm))
    shp.fill.solid()
    shp.fill.fore_color.rgb = fill_rgb
    if line_rgb is None:
        shp.line.fill.background()
    else:
        shp.line.color.rgb = line_rgb
        shp.line.width = Pt(line_w_pt)
    shp.shadow.inherit = False
    return shp


def add_textbox(slide, x_mm, y_mm, w_mm, h_mm,
                text, font_size_pt, font_color, *,
                bold=False, italic=False,
                align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP,
                font_name='Calibri'):
    tb = slide.shapes.add_textbox(Mm(x_mm), Mm(y_mm), Mm(w_mm), Mm(h_mm))
    tf = tb.text_frame
    tf.margin_left   = Mm(4)
    tf.margin_right  = Mm(4)
    tf.margin_top    = Mm(2)
    tf.margin_bottom = Mm(2)
    tf.word_wrap     = True
    tf.vertical_anchor = anchor

    lines = text.split('\n') if isinstance(text, str) else text
    for i, line in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        r = p.add_run()
        r.text = line
        r.font.size      = Pt(font_size_pt)
        r.font.color.rgb = font_color
        r.font.bold      = bold
        r.font.italic    = italic
        r.font.name      = font_name
    return tb


def add_bullets(slide, x_mm, y_mm, w_mm, h_mm,
                bullets, font_size_pt, *,
                font_color=BODY_TEXT, font_name='Calibri'):
    """Add a textbox with real disc-bullet list items (OXML buChar)."""
    tb = slide.shapes.add_textbox(Mm(x_mm), Mm(y_mm), Mm(w_mm), Mm(h_mm))
    tf = tb.text_frame
    tf.margin_left   = Mm(4)
    tf.margin_right  = Mm(4)
    tf.margin_top    = Mm(3)
    tf.margin_bottom = Mm(3)
    tf.word_wrap     = True
    tf.vertical_anchor = MSO_ANCHOR.TOP

    for i, text in enumerate(bullets):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = PP_ALIGN.LEFT
        # python-pptx has no high-level bullet API; emit OXML directly.
        pPr = p._p.get_or_add_pPr()
        pPr.set('indent', str(-228600))   # -0.25" hanging
        pPr.set('marL',   str(228600))    #  0.25" left margin
        buChar = etree.SubElement(pPr, qn('a:buChar'))
        buChar.set('char', '•')
        buFont = etree.SubElement(pPr, qn('a:buFont'))
        buFont.set('typeface', 'Arial')

        r = p.add_run()
        r.text = text
        r.font.size      = Pt(font_size_pt)
        r.font.color.rgb = font_color
        r.font.name      = font_name
    return tb


def add_section(slide, x_mm, y_mm, w_mm, body_h_mm, *,
                heading, body_kind='bullets', body_data=None,
                heading_pt=52, body_pt=28):
    """One section = navy header bar + light-blue body card.

    body_kind: 'bullets' (list[str]) or 'figure' (dict with keys
    'figure_label' + 'caption').

    Returns the y-coordinate immediately below the body, for stacking.
    """
    # Header bar
    add_rect(slide, x_mm, y_mm, w_mm, SEC_HEADER_H, NAVY_MID)
    add_textbox(slide, x_mm, y_mm, w_mm, SEC_HEADER_H,
                heading, heading_pt, WHITE,
                bold=True, align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    # Body card
    body_y = y_mm + SEC_HEADER_H
    add_rect(slide, x_mm, body_y, w_mm, body_h_mm, LIGHT_BLUE,
             line_rgb=BORDER_BLUE, line_w_pt=0.5)
    if body_kind == 'bullets':
        add_bullets(slide, x_mm, body_y + 2, w_mm, body_h_mm - 4,
                    body_data, body_pt)
    elif body_kind == 'figure':
        fig_w = w_mm - 20
        fig_h = body_h_mm - 30
        add_rect(slide, x_mm + 10, body_y + 6, fig_w, fig_h, PLACEHOLDER,
                 line_rgb=BORDER_BLUE, line_w_pt=0.75)
        add_textbox(slide, x_mm + 10, body_y + 6, fig_w, fig_h,
                    body_data['figure_label'], 48, NAVY_DARK,
                    bold=True, align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        add_textbox(slide, x_mm, body_y + 6 + fig_h + 2, w_mm, 18,
                    body_data['caption'], 22, MUTED_TEXT,
                    italic=True, align=PP_ALIGN.CENTER)
    return body_y + body_h_mm


# ────────────────────────────────────────────────────────────────────────
# build the poster
# ────────────────────────────────────────────────────────────────────────

def build(out_path: str) -> None:
    prs = Presentation()
    prs.slide_width  = Mm(PAGE_W_MM)
    prs.slide_height = Mm(PAGE_H_MM)
    slide = prs.slides.add_slide(prs.slide_layouts[6])   # blank

    # ── Title band ──
    add_rect(slide, 0, 0, PAGE_W_MM, TITLE_BAND_H, NAVY_DARK)
    add_textbox(slide, 0, 12, PAGE_W_MM, TITLE_BAND_H - 20,
                "Title Here", 100, WHITE, bold=True,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    # ── Authors / affiliations ──
    auth_y = TITLE_BAND_H
    add_rect(slide, 0, auth_y, PAGE_W_MM, AUTHOR_BAND_H, NAVY_MID)
    add_textbox(slide, 0, auth_y + 4, PAGE_W_MM, 22,
                "Author One¹, Author Two², Author Three³",
                40, WHITE,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.TOP)
    add_textbox(slide, 0, auth_y + 28, PAGE_W_MM, 20,
                "¹Affiliation One, City, State, Country;  "
                "²Affiliation Two, City, State, Country;  "
                "³Affiliation Three, City, State, Country  ·  "
                "presenter@example.org",
                22, WHITE,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.TOP)

    # ── 3-column geometry ──
    col_y0 = TITLE_BAND_H + AUTHOR_BAND_H + 18
    col_x = [
        MARGIN_SIDE_MM,
        MARGIN_SIDE_MM + COL_W + GUTTER,
        MARGIN_SIDE_MM + 2 * (COL_W + GUTTER),
    ]

    # ── LEFT column: Introduction + Objectives ──
    y = col_y0
    y = add_section(slide, col_x[0], y, COL_W, 360,
        heading="Introduction",
        body_data=[
            "Your text goes here. Replace this placeholder with your content.",
            "Font: Calibri 28 pt body. Headings Calibri Bold.",
            "Use this column to introduce your topic, motivate the problem, "
            "and summarise prior work in 3–6 short paragraphs or bullets.",
            "Keep prose tight — posters reward concision over completeness.",
        ])
    y += SEC_GAP
    y = add_section(slide, col_x[0], y, COL_W, 250,
        heading="Objectives",
        body_data=[
            "State 2–4 clear, testable objectives or hypotheses.",
            "Each objective should be a single sentence.",
            "Frame the contribution in terms readers can evaluate at a glance.",
            "Avoid jargon unless it is the focus of the work.",
        ])

    # ── CENTER column: Methods + Figures 1–3 ──
    y = col_y0
    y = add_section(slide, col_x[1], y, COL_W, 80,
        heading="Methods",
        body_data=[
            "Describe your methods, materials, and procedures.",
            "Include key details that let others reproduce the approach.",
            "Bullets and a flowchart usually beat dense paragraphs.",
        ])
    y += 6
    for label in ("Figure 1", "Figure 2", "Figure 3"):
        y = add_section(slide, col_x[1], y, COL_W, 220,
            heading=label,
            body_kind='figure',
            body_data={'figure_label': label,
                       'caption': f"{label}. Caption goes here. "
                                  f"Calibri 22 pt italic."})
        y += SEC_GAP

    # ── RIGHT column ──
    y = col_y0
    y = add_section(slide, col_x[2], y, COL_W, 220,
        heading="Results",
        body_data=[
            "Your text goes here. Replace this placeholder with your content.",
            "Present your key findings and results.",
            "Use text, tables, and figures to communicate your results "
            "effectively.",
            "Highlight effect sizes and confidence intervals where applicable.",
        ])
    y += SEC_GAP
    y = add_section(slide, col_x[2], y, COL_W, 200,
        heading="Conclusions",
        body_data=[
            "Summarise your main conclusions in 3–5 short takeaways.",
            "Discuss the implications of your findings.",
            "Identify open questions surfaced by this work.",
        ])
    y += SEC_GAP
    y = add_section(slide, col_x[2], y, COL_W, 130,
        heading="Future Work",
        body_data=[
            "List concrete next steps and outstanding questions.",
            "Mention planned follow-up experiments or datasets.",
        ])
    y += SEC_GAP
    y = add_section(slide, col_x[2], y, COL_W, 110,
        heading="Acknowledgements",
        body_data=[
            "Acknowledge funding sources, contributors, "
            "and supporting organisations.",
        ])
    y += SEC_GAP

    # ── References + Contact, side-by-side in the right column ──
    half_w = (COL_W - 8) / 2
    add_rect(slide, col_x[2], y, half_w, SEC_HEADER_H, NAVY_MID)
    add_textbox(slide, col_x[2], y, half_w, SEC_HEADER_H,
                "References", 44, WHITE, bold=True,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    add_rect(slide, col_x[2], y + SEC_HEADER_H, half_w, 110,
             LIGHT_BLUE, line_rgb=BORDER_BLUE, line_w_pt=0.5)
    add_bullets(slide, col_x[2], y + SEC_HEADER_H + 2, half_w, 106,
        [
            "[1] First A. et al. (YEAR). Title. Venue.",
            "[2] Second B. et al. (YEAR). Title. Venue.",
            "[3] Third C. et al. (YEAR). Title. Venue.",
        ], 18)

    contact_x = col_x[2] + half_w + 8
    add_rect(slide, contact_x, y, half_w, SEC_HEADER_H, NAVY_MID)
    add_textbox(slide, contact_x, y, half_w, SEC_HEADER_H,
                "Contact", 44, WHITE, bold=True,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    add_rect(slide, contact_x, y + SEC_HEADER_H, half_w, 110,
             LIGHT_BLUE, line_rgb=BORDER_BLUE, line_w_pt=0.5)

    qr_size = 60
    add_rect(slide, contact_x + 6, y + SEC_HEADER_H + 12, qr_size, qr_size,
             WHITE, line_rgb=NAVY_DARK, line_w_pt=1.5)
    add_textbox(slide, contact_x + 6, y + SEC_HEADER_H + 12, qr_size, qr_size,
                "QR\nCode\nHere", 16, MUTED_TEXT,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    add_bullets(slide, contact_x + qr_size + 12,
                y + SEC_HEADER_H + 8, half_w - qr_size - 18, 90,
        [
            "Your Name",
            "Your Institution",
            "you@domain.com",
            "www.yourwebsite.com",
        ], 18)

    # ── Footer (print guidance) ──
    add_textbox(slide, MARGIN_SIDE_MM, PAGE_H_MM - 28,
                PAGE_W_MM - 2 * MARGIN_SIDE_MM, 16,
                "Poster size: A0 portrait (841 × 1189 mm).  "
                "Layout: 3 columns × 260 mm with 18 mm gutters.  "
                "Print: PDF/X-1a, 300 dpi, CMYK if required by printer; "
                "3–5 mm bleed; 2 mm safety margin.",
                14, MUTED_TEXT, italic=True,
                align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    prs.save(out_path)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Generate a navy/light-blue A0-portrait conference "
                    "poster scaffold (PPTX).")
    ap.add_argument(
        '-o', '--output',
        default='conference-poster.pptx',
        help="Output path (default: ./conference-poster.pptx). "
             "Tilde-expands.")
    args = ap.parse_args()

    out = os.path.expanduser(args.output)
    out_dir = os.path.dirname(out)
    if out_dir and not os.path.isdir(out_dir):
        print(f"error: output directory does not exist: {out_dir}",
              file=sys.stderr)
        return 1

    build(out)
    print(f"saved: {out}")
    print(f"  dims: {PAGE_W_MM} × {PAGE_H_MM} mm (A0 portrait)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
