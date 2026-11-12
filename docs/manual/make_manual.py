#!/usr/bin/env python3
"""KIST's manual as a PDF (docs/manual/KIST-manual.pdf), for players rather than programmers.

A5, the Van de Graaf canon's margins (the text block a ninth in from the top and the spine, two ninths from the
foot and the fore-edge, facing pages mirrored), set in the screen's own faces: IM Fell English and, for the titles,
UnifrakturMaguntia (both SIL OFL 1.1, assets/fonts).

    python3 docs/manual/make_manual.py SCREENSHOT_DIR [OUT.pdf]

SCREENSHOT_DIR holds the host simulator's shots (tests/host/basic.txt, and the temperament shots of
docs/manual/temperaments.txt), each 240 x 240; they are enlarged here by whole pixels.

KIST-P's manual (docs/manual/KIST-P-manual.pdf): KIST_P=1, and a third argument, the directory of KIST-P's own shots
(docs/manual/screens-p.txt), which stand in for KIST's where both have one.

    KIST_P=1 python3 docs/manual/make_manual.py build/manual docs/manual/KIST-P-manual.pdf build/manual-p"""
import os
import sys
from pathlib import Path

from PIL import Image
from reportlab.lib.colors import Color
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A5
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, CondPageBreak, Frame, Image as RLImage, KeepTogether, NextPageTemplate,
                                PageBreak, PageTemplate, Paragraph, Spacer, Table, TableStyle)

ROOT = Path(__file__).resolve().parents[2]
FONTS = ROOT / "assets" / "fonts"
SHOTS = Path(sys.argv[1] if len(sys.argv) > 1 else "build/host")
PLATE = os.environ.get("KIST_P") == "1"
NAME = "KIST-P" if PLATE else "KIST"
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "docs" / "manual" / f"{NAME}-manual.pdf")
SHOTS_P = Path(sys.argv[3]) if len(sys.argv) > 3 else None   # KIST-P's own shots, before KIST's
TMP = Path("/tmp/kist-manual-img")

pdfmetrics.registerFont(TTFont("Fell", str(FONTS / "IMFeENrm28P.ttf")))
pdfmetrics.registerFont(TTFont("Fell-It", str(FONTS / "IMFeENit28P.ttf")))
pdfmetrics.registerFont(TTFont("Fell-SC", str(FONTS / "IMFeENsc28P.ttf")))
pdfmetrics.registerFont(TTFont("Fraktur", str(FONTS / "UnifrakturMaguntia-Book.ttf")))
pdfmetrics.registerFontFamily("Fell", normal="Fell", italic="Fell-It", bold="Fell", boldItalic="Fell-It")

INK = Color(30 / 255, 26 / 255, 22 / 255)
RED = Color(158 / 255, 42 / 255, 31 / 255)
MID = Color(107 / 255, 98 / 255, 82 / 255)
RULE = Color(184 / 255, 174 / 255, 152 / 255)

W, H = A5
INNER, TOP, OUTER, FOOT = W / 9, H / 9, 2 * W / 9, 2 * H / 9   # the canon
TW, TH = W - INNER - OUTER, H - TOP - FOOT

body = ParagraphStyle("body", fontName="Fell", fontSize=10.5, leading=13.6, textColor=INK, alignment=TA_JUSTIFY,
                      spaceAfter=5)
first = ParagraphStyle("first", parent=body)
lead = ParagraphStyle("lead", parent=body, fontName="Fell-It", fontSize=11.5, leading=15, alignment=TA_LEFT,
                      spaceAfter=8)
h1 = ParagraphStyle("h1", fontName="Fraktur", fontSize=24, leading=28, textColor=INK, spaceAfter=4)
h2 = ParagraphStyle("h2", fontName="Fell-SC", fontSize=12, leading=15, textColor=RED, spaceBefore=9, spaceAfter=3)
cap = ParagraphStyle("cap", fontName="Fell-It", fontSize=9, leading=11.5, textColor=MID, alignment=TA_CENTER,
                     spaceBefore=3, spaceAfter=9)
cell = ParagraphStyle("cell", fontName="Fell", fontSize=9.5, leading=12, textColor=INK)
cellh = ParagraphStyle("cellh", fontName="Fell-SC", fontSize=9.5, leading=12, textColor=RED)
bullet = ParagraphStyle("bullet", parent=body, leftIndent=11, bulletIndent=0, spaceAfter=3, alignment=TA_LEFT)
note = ParagraphStyle("note", parent=body, fontName="Fell-It", fontSize=9.5, leading=12.5, textColor=MID,
                      alignment=TA_LEFT)


def page_deco(canvas, doc, recto):
    canvas.saveState()
    x0 = INNER if recto else OUTER
    n = canvas.getPageNumber()
    if n > 1:
        canvas.setFont("Fell-SC", 8.5)
        canvas.setFillColor(MID)
        head = f"{NAME}  ·  a chamber organ for the FM-1" if recto else getattr(doc, "chapter", "")
        y = H - TOP + 14
        if recto:
            canvas.drawRightString(x0 + TW, y, head)
        else:
            canvas.drawString(x0, y, head)
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.4)
        canvas.line(x0, y - 4, x0 + TW, y - 4)
        canvas.setFont("Fell", 10)
        canvas.setFillColor(INK)
        if recto:
            canvas.drawRightString(x0 + TW, FOOT - 22, str(n))
        else:
            canvas.drawString(x0, FOOT - 22, str(n))
    canvas.restoreState()


class Doc(BaseDocTemplate):
    def __init__(self, path):
        super().__init__(str(path), pagesize=A5, title=f"{NAME}: a chamber organ for the M-VAVE FM-1",
                         author="KIST", subject="The player's manual")
        recto = Frame(INNER, FOOT, TW, TH, id="r", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
        verso = Frame(OUTER, FOOT, TW, TH, id="v", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
        self.addPageTemplates([
            PageTemplate("recto", [recto], onPage=lambda c, d: page_deco(c, d, True)),
            PageTemplate("verso", [verso], onPage=lambda c, d: page_deco(c, d, False)),
        ])
        self.chapter = ""

    def handle_pageBegin(self):
        """odd pages are rectos, even ones versos, whatever broke the page"""
        self.pageTemplate = self.pageTemplates[0 if (self.page + 1) % 2 else 1]
        super().handle_pageBegin()

    def afterFlowable(self, f):
        if isinstance(f, Paragraph) and f.style.name == "h1":
            self.chapter = f.getPlainText()


def shot(name, caption, width=58 * mm, keep=True):
    """a screenshot, enlarged by whole pixels so it stays sharp, with a hairline round it"""
    TMP.mkdir(exist_ok=True)
    src = SHOTS / f"{name}.png"
    if SHOTS_P and (SHOTS_P / f"{name}.png").exists():
        src = SHOTS_P / f"{name}.png"
    big = TMP / f"{name}.png"
    Image.open(src).convert("RGB").resize((960, 960), Image.NEAREST).save(big)
    img = RLImage(str(big), width=width, height=width)
    t = Table([[img]], colWidths=[width + 2], rowHeights=[width + 2])
    t.setStyle(TableStyle([("BOX", (0, 0), (-1, -1), 0.4, MID), ("ALIGN", (0, 0), (-1, -1), "CENTER"),
                           ("VALIGN", (0, 0), (-1, -1), "MIDDLE"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                           ("RIGHTPADDING", (0, 0), (-1, -1), 0), ("TOPPADDING", (0, 0), (-1, -1), 0),
                           ("BOTTOMPADDING", (0, 0), (-1, -1), 0)]))
    parts = [t, Paragraph(caption, cap)]
    return KeepTogether(parts) if keep else parts


def pair(a, ca, b, cb):
    """two screenshots side by side"""
    w = (TW - 6 * mm) / 2
    t = Table([[shot(a, ca, w, False), shot(b, cb, w, False)]], colWidths=[TW / 2, TW / 2])
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 0),
                           ("RIGHTPADDING", (0, 0), (-1, -1), 0)]))
    return t


def table(rows, widths, head=True):
    data = [[Paragraph(c, cellh if (head and i == 0) else cell) for c in r] for i, r in enumerate(rows)]
    t = Table(data, colWidths=[w * TW for w in widths], repeatRows=1 if head else 0)
    st = [("VALIGN", (0, 0), (-1, -1), "TOP"), ("LEFTPADDING", (0, 0), (-1, -1), 2),
          ("RIGHTPADDING", (0, 0), (-1, -1), 4), ("TOPPADDING", (0, 0), (-1, -1), 2.5),
          ("BOTTOMPADDING", (0, 0), (-1, -1), 2.5), ("LINEBELOW", (0, 0), (-1, -2), 0.3, RULE)]
    if head:
        st.append(("LINEBELOW", (0, 0), (-1, 0), 0.6, INK))
    t.setStyle(TableStyle(st))
    return KeepTogether([t, Spacer(1, 7)])


def P(t, s=body):
    return Paragraph(t, s)


def H1(t):
    return [CondPageBreak(TH * 0.6), Paragraph(t, h1)]


def B(t):
    return Paragraph(t, bullet, bulletText="·")


def red(t):
    return f'<font color="#9e2a1f">{t}</font>'


def sc(t):
    return f'<font name="Fell-SC">{t}</font>'


S = []
# ---------------------------------------------------------------------------------------------------- cover ---
S += [Spacer(1, 22 * mm),
      Paragraph('<font color="#9e2a1f">K</font>IST' + ("-P" if PLATE else ""), ParagraphStyle("t", fontName="Fraktur", fontSize=64, leading=70,
                                                                    alignment=TA_CENTER, textColor=INK)),
      Spacer(1, 4 * mm),
      Paragraph("a chamber organ for the M-VAVE FM-1", ParagraphStyle("st", fontName="Fell-It", fontSize=14, leading=18,
                                                                      alignment=TA_CENTER, textColor=INK)),
      Spacer(1, 10 * mm),
      Paragraph("The Player’s Manual", ParagraphStyle("st2", fontName="Fell-SC", fontSize=12, leading=16,
                                                      alignment=TA_CENTER, textColor=RED)),
      Spacer(1, 16 * mm),
      Paragraph("Four wooden stops · forty-eight pipes speaking at once · the wind, " + ("the plate" if PLATE else "the room")
                + " and the old temperaments",
                ParagraphStyle("st3", fontName="Fell", fontSize=10, leading=14, alignment=TA_CENTER, textColor=MID)),
      PageBreak()]

# ------------------------------------------------------------------------------------------------ the organ ---
S += H1("The Instrument")
if PLATE:
    S += [P("KIST-P is KIST with its rooms taken out and a plate put in: a large sheet of steel hung in a frame, "
            "set ringing by a driver and heard through two pickups, as studios have used since the 1950s for an "
            "echo that can be made to measure. Every part of it is yours to set, on three pages of its own. The "
            "organ itself is the same as KIST’s in every way.", lead)]
S += [P("KIST turns the FM-1 into a small wooden pipe organ of the kind that sits beside a harpsichord in a baroque "
        "ensemble: a box organ, or <i>kistorgel</i> as the Dutch call it, built in the manner of the continuo organs "
        "of the Klop workshop. It has one keyboard, four stops and nothing else, and it is meant to be played rather "
        "than programmed.", lead),
      P("Nothing you hear is a recording. An ordinary digital organ plays back samples: recordings of real pipes, the "
        "same recording every time a key goes down. KIST does something different. It works out, many thousands of "
        "times a second, what the air is doing inside every pipe that is speaking: the jet of wind at the mouth, the "
        "column of air in the pipe, the way the two push each other into a steady tone. The sound is the result of "
        "that calculation, made fresh for every note."),
      P("That has consequences you can hear and feel:"),
      B("<b>Every note starts a little differently.</b> A pipe takes a moment to settle, with a small breathy "
        "‘chiff’, and how it settles depends on how quickly you press the key."),
      B("<b>The pipes share their wind.</b> Hold a big chord and the wind sags a touch, as it does in a real organ "
        "whose bellows are working hard; switch the blower off and the notes die as the reservoir empties."),
      B("<b>No two pipes are quite alike.</b> Each is tuned a hair’s breadth away from perfect, as real pipes are, "
        "so a chord has life in it rather than the glassy stillness of a machine."),
      B("<b>The tuning is truly the old tuning.</b> When you choose meantone or Werckmeister, the pipes themselves "
        "are tuned to it; you are not hearing a modern organ bent out of shape."),
      P("The keyboard runs from the C two octaves below middle C to the F above the C two octaves above it: 54 keys, "
        "the compass of a small continuo organ. The FM-1’s own keys play the middle of it; a larger keyboard "
        "plugged in over MIDI reaches the rest (see <i>Playing from another keyboard</i>).")]

S += [P("The four stops", h2),
      P("A stop is a rank of pipes, one for each key. Pulling a stop out lets the wind into its rank; on KIST, "
        "pressing its button does the same, and the button lights while the stop is drawn."),
      table([["Stop", "Pitch", "Its sound"],
             ["Gedackt", "8 foot", "Stopped wooden pipes, capped at the top: soft, round and flute-like, sounding at "
              "the pitch you play. The foundation of everything; on its own, the classic continuo sound."],
             ["Rohrflöte", "4 foot", "A chimney flute: a stopped pipe with a little open tube in its cap, which gives "
              "it a brighter, slightly nasal edge. It sounds an octave higher than you play."],
             ["Principal", "2 foot", "Open wooden pipes, two octaves up: clear and singing. It adds brilliance and "
              "makes the organ carry."],
             ["Quinta", "2⅔ foot", "Open pipes sounding a twelfth higher (an octave and a fifth). Only in the treble. "
              "It is a colour stop: on its own it sounds odd, with the Gedackt it blends into a reedy, "
              "singing solo voice."]],
            [0.22, 0.16, 0.62]),
      P("“Foot” is the organ builder’s old way of naming pitch: an 8-foot pipe is the length that sounds the note "
        "you play at the bottom of the keyboard; a 4-foot pipe, half as long, sounds an octave higher; and so on."),
      P("Bass and treble", h2),
      P("Every stop is divided between the bass half of the keyboard and the treble half, and each half has its own "
        "button. This is how the small Dutch organs were built, and it is very useful: you can accompany softly in "
        "the left hand on the Gedackt alone while the right hand plays a brighter solo with the Principal or the "
        "Quinta added. The division falls between B and middle C, and can be moved (see <i>The Works</i>)."),
      P("Some registrations to start with", h2),
      table([["For", "Draw"],
             ["Continuo, accompanying voices or strings", "Gedackt 8 alone, both halves"],
             ["A little more presence", "Gedackt 8 and Rohrflöte 4"],
             ["Full organ for a chorale", "Gedackt 8, Rohrflöte 4, Principal 2"],
             ["A solo in the right hand", "Gedackt 8 and Quinta in the treble, Gedackt 8 alone in the bass"],
             ["A bright, silvery solo", "Gedackt 8 and Principal 2 in the treble"]],
            [0.55, 0.45])]

# ----------------------------------------------------------------------------------------------- the panel ---
S += H1("Finding your Way")
S += [P("All of " + NAME + " is played from the FM-1’s own buttons and knobs. The words printed on the panel belong to "
        "the FM-1’s original firmware; under " + NAME + " they do these things:", lead),
      P("The buttons", h2),
      table([["Button", "In " + NAME],
             [sc("fx") + " / " + sc("sel"), "Gedackt 8, bass / treble"],
             [sc("env") + " / " + sc("lfo"), "Rohrflöte 4, bass / treble"],
             [sc("edit") + " / " + sc("glo"), "Principal 2, bass / treble"],
             [sc("arp"), "Quinta 2⅔ (treble only)"],
             [sc("rec"), "Cancel: put every stop in at once. Press again to bring them all back."],
             [sc("seq"), "The tremulant: a gentle, regular wavering of the wind"],
             [sc("play"), "The blower. Green while it runs."],
             [sc("save"), "Keep the present setup for the next time the FM-1 is switched on. Lit while something "
              "has changed and is not yet kept."],
             [sc("home"), "Back to the first page, the Console"],
             [sc("oct-") + " / " + sc("oct+"), "Move the FM-1’s own keys down or up an octave; both together put "
              "them back"]],
            [0.3, 0.7]),
      P("The knobs", h2),
      table([["Knob", "In " + NAME],
             [sc("select"), "Turn the pages of the screen"],
             [sc("presets"), ("How much of the plate you hear (the mix)" if PLATE else
                              "How much of the room you hear (the room mix)") + ", from any page"],
             [sc("algorithm"), "Choose the temperament, from any page"],
             [sc("master"), "The volume"],
             ["The four knobs " + sc("k1–k4"), "Whatever the page shows at its foot, in the same arrangement as the "
              "knobs themselves"]],
            [0.3, 0.7]),
      P("Reading the screen", h2),
      P("Every page is laid out the same way, like a page from an old printed book. At the top is its title in "
        "black-letter, and under it a line in italics that says what the page is about. When you turn a knob, that "
        "line briefly shows, in red, what you have just changed."),
      P("The middle of the page is its picture: the stops, the tuning, " + ("the plate" if PLATE else "the room") + ", the wind. At the foot is a small "
        "table of four boxes, numbered i to iv. They stand for the four knobs, in the same places: i is the "
        "first knob, ii the second, and so on. Each box has the knob’s name in small red letters and, under it, "
        "what it is set to now."),
      shot("console", "The Console page. Each round head is a stop half: filled when drawn. The Quinta has no bass "
                      "half, so its left head is dotted.")]

# ------------------------------------------------------------------------------------------------- pages ---
S += H1("The Pages")
S += [P("Turn " + sc("select") + " to move through the " + ("eight" if PLATE else "six") + " pages. " + sc("home")
        + " always brings you back to the first.", lead),
      P("i · The Console", h2),
      P("The organ’s front: the four stops with their bass and treble halves, and the four things you are most "
        "likely to want while playing."),
      table([["Knob", "Sets"],
             ["i · Decay", "How long the plate rings, in seconds (see <i>The Plate</i>)"] if PLATE else
             ["i · Room", "Which room the organ stands in (see <i>The Room</i>)"],
             ["ii · its share", "How much of the " + ("plate" if PLATE else "room")
              + " you hear, from 0 (the pipes alone) to 100"],
             ["iii · Pitch", "The pitch of A (see <i>Tuning</i>)"],
             ["iv · Touch", "How the keys respond, from <i>soft</i> to <i>crisp</i> (see <i>The Wind</i>)"]],
            [0.3, 0.7]),
      P("ii · Tuning", h2),
      P("Where the organ is tuned, and how. This page has its own chapter, <i>Tuning</i>, below.")]
if PLATE:
    S += [P("iii · The Plate", h2),
          P("An organ is never heard on its own: the building is half the instrument. KIST-P gives it a plate instead: "
            "the picture is the plate in its frame, hung on four springs, with the driver (the red ring) that sets it "
            "moving, the two pickups that hear it, and a felt damper that closes on it as you take the treble out. "
            "The plate grows and shrinks with its size. Under it, the curve is how the sound dies away over six "
            "seconds."),
          shot("plate", "The Plate. The drawing follows the size and the damping; the curve below is the decay."),
          table([["Knob", "Sets"],
                 ["i · Size", "How large the plate is, 30 to 130 %. A larger plate is darker and its echoes are "
                  "further apart. The size glides rather than jumps, so turning it while the plate rings bends the "
                  "pitch of the tail, a sound no real plate can make."],
                 ["ii · Decay", "How long the plate rings, 0.2 to 12 seconds. It holds while the size glides."],
                 ["iii · Damping", "Where the treble starts to die sooner than the bass, 1 to 10 kHz: low is a "
                  "dark, felted plate, high a bright, bare one"],
                 ["iv · Pre-delay", "A pause before the plate answers, 0 to 100 ms: a little keeps the pipes’ "
                  "attack clear of the echo"]],
                [0.3, 0.7]),
          P("iv · The Drive", h2),
          P("What goes into the plate, and how it spreads. The curve is the plate’s colour, bass on the left, "
            "treble on the right."),
          shot("drive", "The Drive. The curve shows the bandwidth going in and the low cut."),
          table([["Knob", "Sets"],
                 ["i · Bandwidth", "How much treble reaches the plate, 1 to 10 kHz"],
                 ["ii · Input diffusion", "How quickly a note is smeared into a wash as it enters: low keeps "
                  "some separate early echoes"],
                 ["iii · Decay diffusion", "How dense the tail is: low, it grows grainy and you hear its echoes"],
                 ["iv · Low cut", "Takes the deep bass out of the plate, off to 500 Hz, so the Gedackt’s low notes "
                  "don’t boom"]],
                [0.3, 0.7]),
          P("v · The Motion", h2),
          P("A real plate is never quite still, and a little movement keeps a long tail from ringing on one "
            "note. The sine shows how fast and how far it moves."),
          shot("motion", "The Motion: the rate and depth of the plate’s wander, its width and the mix."),
          table([["Knob", "Sets"],
                 ["i · Rate", "How fast the plate wanders, 0.1 to 3 times a second"],
                 ["ii · Depth", "How far, from still (0) to 100. Deep and fast makes a chorus of it."],
                 ["iii · Width", "From one point in the middle (0) to the full width of the two pickups (100)"],
                 ["iv · Mix", "How much of the plate you hear, as " + sc("presets") + " sets it"]],
                [0.3, 0.7])]
else:
    S += [P("iii · The Room", h2),
      P("An organ is never heard on its own: the building is half the instrument. KIST has five rooms, from a dry, "
        "close space to a large stone abbey, each with its own size and echo."),
      shot("room", "The Room. The chosen room is marked ¶ in red; the drawing on the right is how its sound dies "
                   "away over three seconds."),
      table([["Knob", "Sets"],
             ["i · Room", "<i>Dry</i>, <i>Chapel</i>, <i>Village church</i>, <i>Abbey nave</i> or <i>Concert hall</i>. "
              "Choosing a room also sets its natural decay."],
             ["ii · Blower", "The faint hum and rush of the organ’s blower, from silent to 10. Real organs are never "
              "quite silent; a little blower noise makes KIST sound like a thing in a room."],
             ["iii · Distance", "How far away you sit, 1 to 20 metres: further away, the room takes over from the "
              "pipes."],
             ["iv · Decay", "How long the echo lasts, in seconds"]],
            [0.3, 0.7])]
S += [P(("vi" if PLATE else "iv") + " · The Wind", h2),
      P("The wind is what makes an organ breathe. KIST models a reservoir of air, filled by the blower and drawn on by "
        "the pipes. The page shows the reservoir as a pair of bellows and a number (100 is the pressure the pipes were "
        "voiced for), and under it a trace of the last six seconds, so you can watch the wind dip when you play a "
        "full chord, or rock with the tremulant."),
      shot("wind_trem", "The Wind, with the tremulant beating. The line under the title tells you how hard the last key "
                        "was struck and how quickly its pipe valve opened."),
      table([["Knob", "Sets"],
             ["i · Wind", "The pressure, a little above or below the voiced pressure. Less wind makes the pipes "
              "softer and a touch flatter, more makes them firmer and sharper, as on a real organ."],
             ["ii · Steadiness", "<i>steady</i>, <i>lively</i> or <i>loose</i>: how much the wind gives way when "
              "many pipes speak at once"],
             ["iii · Tremulant", "How fast the tremulant beats, in beats a second"],
             ["iv · its depth", "<i>gentle</i>, <i>moderate</i> or <i>deep</i>"]],
            [0.3, 0.7]),
      P(("vii" if PLATE else "v") + " · Intonation", h2),
      P("Intonation is what an organ builder does after the pipes are made: going through them one by one, making "
        "each sit well with its neighbours. KIST lets you do the same. Choose a stop with the first knob, then strike "
        "a key: that pipe is marked in red on the chart, and the second knob makes it louder or softer."),
      shot("voicing", "Intonation. Each mark is one pipe of the chosen stop; above the line is louder, below softer. "
                      "The red upright line is the division between bass and treble."),
      table([["Knob", "Sets"],
             ["i · Register", "Which stop you are working on"],
             ["ii · (the note)", "How loud the last pipe you struck is, in half-decibel steps"],
             ["iii · Chiff", "How much of a breathy start the stop’s pipes have, from none to 4"],
             ["iv · Drift", "How far each pipe is allowed to wander from perfect tuning, in cents (hundredths of a "
              "semitone). A little drift makes chords sound alive; none makes them sound electronic."]],
            [0.3, 0.7]),
      P(("viii" if PLATE else "vi") + " · The Works", h2),
      P("The organ’s workings: things you set once and leave."),
      shot("system", "The Works. The line under the title shows how hard the FM-1 is working, and how many pipes are "
                     "speaking."),
      table([["Knob", "Sets"],
             ["i · Channel", "Which MIDI channel KIST listens to, or <i>any</i>"],
             ["ii · Division", "Where the bass ends and the treble begins"],
             ["iii · Transpose", "Shift everything you play over MIDI up or down by semitones, for music written at a "
              "different pitch"]] + ([["iv · Blower", "The faint hum and rush of the organ’s blower, from silent to "
                                       "10 (on KIST it is on the Room page)"]] if PLATE else []),
            [0.3, 0.7]),
      P("The page also says which memory chip your FM-1 has (there are two kinds, and KIST knows both), and whether "
        "the FM-1’s second processor is helping. Most FM-1s have a second processor that KIST uses for the highest "
        "pipes and the " + ("plate" if PLATE else "room") + ". On some units it is unreliable; KIST notices, stops using it and carries on "
        "with one, and the page then says <i>retired</i>. You will not hear the difference in ordinary playing.", note)]

# ------------------------------------------------------------------------------------------------ tuning ---
S += H1("Tuning")
S += [P("Two questions decide how an organ is tuned: how high it is pitched, and how the twelve notes of the octave "
        "are shared out between them. KIST lets you choose both, the way organs were tuned when this music was "
        "new.", lead),
      P("The pitch of A", h2),
      P("Today an orchestra tunes its A to 440 vibrations a second. It was not always so. Baroque ensembles commonly "
        "play at 415, a semitone lower; French baroque music often at 392, a tone lower; some Venetian music at "
        "465. KIST can be set anywhere from 300 to 500.99. Turning the first knob on the Tuning page moves in whole "
        "numbers (a quick turn jumps by ten) and pauses for a moment at 392, 415, 432, 440 and 465, so the usual "
        "pitches are easy to find. The second knob sets the hundredths, for matching another instrument exactly."),
      P("Temperament", h2),
      P("There is no way to tune twelve notes so that every interval in every key is perfectly pure: the "
        "arithmetic does not close. Something has to give, and a <i>temperament</i> is a decision about where. "
        "Modern equal temperament spreads the error evenly, so every key sounds the same and none sounds quite "
        "sweet. The older temperaments made the common keys beautiful and let the rare ones grow sharp and "
        "colourful. Music written for them sounds as its composers heard it when played in them."),
      table([["Temperament", "What it is like"],
             ["Equal", "Every key alike: the modern piano. Safe for anything, sweet in nothing."],
             ["Werckmeister III", "1691. Pure, bright keys near C; the far keys more restless. Good for Bach and "
              "Buxtehude."],
             ["Meantone", "The Renaissance and early baroque sound: the most beautiful thirds, but only a few keys "
              "usable. One fifth, the ‘wolf’, howls; music in the right keys never goes near it."],
             ["Kirnberger III", "1779. Very pure in the common keys, still playable in all of them."],
             ["Vallotti", "About 1750. Gentle and well balanced: nearly equal, but each key keeps a little of its own "
              "character. A good everyday choice."]],
            [0.3, 0.7]),
      P("Turn " + sc("algorithm") + " from any page to choose. A list appears; KIST waits until the knob has been still "
        "for a moment before retuning, so you can turn past several without the organ jumping about."),
      shot("temperament_overlay", "Choosing a temperament with ALGORITHM."),
      P("Every temperament can also be moved to start on another note with the fourth knob (<i>upon</i>), as "
        "tuners did to favour the keys a piece is written in."),
      P("Reading the clock", h2),
      P("The Tuning page draws the octave as a clock face, with C at twelve o’clock and the other eleven notes round "
        "the dial in order. The short ticks outside the ring are where equal temperament puts each note. The dotted "
        "hands are where the temperament you have chosen puts them. Where a hand points straight at its tick, that "
        "note is just as in equal temperament; where it leans away, the note is higher (clockwise) or lower. The "
        "differences are small, so the clock draws them three times larger than they are."),
      pair("t_equal", "Equal: every hand on its tick.", "t_mean", "Meantone: the hands lean far, G♯ and E♭ most."
           .replace("♯", "#").replace("♭", "b")),
      pair("t_werck", "Werckmeister III.", "t_vall", "Vallotti: smaller leans, mostly in the sharps and flats."),
      P("The red hand is the note the temperament is laid out from. Move it with <i>upon</i> and the whole pattern "
        "turns with it."),
      P("Equal divisions", h2),
      P("Choose <i>Equal</i> and the fourth knob sets how many equal steps the octave is divided into, from 5 to 72; "
        "12 is the ordinary keyboard. Other numbers are a door to other musical worlds. With 19 or 31 steps the "
        "thirds come close to meantone’s sweetness while every key works; 31 was proposed for this in the "
        "seventeenth century, and an organ tuned so stood in Haarlem. With other divisions, each key of the keyboard "
        "plays the next step, so the octave no longer falls on the same letter. The clock then shows the steps as "
        "ticks round the dial."),
      shot("tuning_31edo", "Thirty-one equal parts. Every key plays the next of the thirty-one steps.", 50 * mm)]

# ------------------------------------------------------------------------------------------------ playing ---
S += H1("Touch and Wind")
S += [P("On a mechanical organ the key is joined to the pipe valve by a long wooden rod. You cannot make a pipe "
        "louder by striking harder, but you can feel and hear how quickly the valve opens: slowly, and the pipe "
        "speaks gently; quickly, and it speaks with a little more attack.", lead),
      P("KIST works the same way. How hard you strike a key changes how fast its valve opens, not how loud the pipe "
        "is. The fourth knob on the Console sets how strongly your touch matters, from <i>soft</i> (every note "
        "starts gently) to <i>crisp</i> (a firm stroke gives a quick, clear start)."),
      P("Things worth trying:", h2),
      B("Hold a full chord with every stop drawn and watch the Wind page: the reservoir drops a little, and the pitch "
        "with it, as on a real organ. Set <i>Steadiness</i> to <i>loose</i> to exaggerate it."),
      B("Switch the blower off with " + sc("play") + " while holding a chord. The notes go on as long as there is wind "
        "in the reservoir, then sink and die."),
      B("Press " + sc("seq") + " for the tremulant. It is a proper wind tremulant: it shakes the air, so the pitch and "
        "loudness waver together."),
      B("Draw or retire a stop while holding keys. The pipes come in and out as they would on a real slider chest."),
      B("Press " + sc("rec") + " to silence every stop at once (to change registration quickly between pieces), then "
        "press it again to bring them all back."),
      P("Saving", h2),
      P("When you have the organ as you like it, press " + sc("save") + ". The next time the FM-1 is switched on, it "
        "will come back exactly so: the stops, the " + ("plate" if PLATE else "room") + ", the tuning, the wind and your voicing. Saving takes a moment "
        "during which the organ falls silent, so save between pieces rather than during one. While something has "
        "changed and not been saved, the " + sc("save") + " button is lit."),
      P("Playing from another keyboard", h2),
      P("The FM-1’s own keyboard is small. A larger keyboard plugged in by USB or into the FM-1’s MIDI socket plays "
        "the whole 54-key compass. The sustain pedal does nothing, as on an organ: a pipe sounds exactly as long as "
        "its key is held. Velocity from the keyboard works as touch, as above.")]

# ---------------------------------------------------------------------------------------------- install ---
S += H1("Installing and Going Back")
S += [P("KIST replaces the FM-1’s own firmware, and can always be replaced by it again.", lead),
      P("The easiest way is <i>FM-1 Manager</i>, a small program for Mac, Windows and Linux, from the KIST "
        "page on GitHub (github.com/78Hz/kist, under <i>Releases</i>):"),
      B("<b>Before the first install</b>, use FM-1 Manager’s <i>Whole flash</i> tab to keep a complete copy of "
        "the FM-1 as it came from the factory (Mac and Linux)."),
      B("Connect the FM-1 to the computer with a USB cable, switched on as usual."),
      B("In the <i>Firmware</i> tab, choose " + NAME + " and press <i>Install on FM-1</i>. It takes a few minutes; do not "
        "unplug it until it says it has finished."),
      B("To go back, choose M-VAVE’s own firmware the same way (the Manager can fetch it for you)."),
      P("If anything goes wrong during an install, install again: the FM-1 keeps its old firmware until the new one "
        "is safely written. The Manager’s <i>Saved data</i> tab keeps a copy of your saved setup, so a new version "
        "of KIST, or a different FM-1, can have your organ back."),
      P("Credits", h2),
      P("KIST is free software under the Mozilla Public License 2.0, built on fm1-base and felucca-bsp. Its screens "
        "and this manual are set in IM Fell English, cut by Igino Marini after the types Bishop John Fell gave to "
        "Oxford in the seventeenth century, and UnifrakturMaguntia, both under the SIL Open Font License. KIST is "
        "inspired by the box organs of the Dutch Klop workshop and is not connected with it. M-VAVE and FM-1 are "
        "trademarks of their owners; KIST is not affiliated with M-VAVE.", note)]

# ---------------------------------------------------------------------------------------- quick reference ---
S += [PageBreak()]
S += H1("At a Glance")
S += [table([["Page", "i", "ii", "iii", "iv"],
             ["Console", "Decay" if PLATE else "Room", "its share", "Pitch", "Touch"],
             ["Tuning", "A (Hz)", "fine", "Temperament", "upon / Division"],
             *([["The Plate", "Size", "Decay", "Damping", "Pre-delay"],
                ["The Drive", "Bandwidth", "In. diffusion", "Dec. diffusion", "Low cut"],
                ["The Motion", "Rate", "Depth", "Width", "Mix"]] if PLATE else
               [["The Room", "Room", "Blower", "Distance", "Decay"]]),
             ["The Wind", "Wind", "Steadiness", "Tremulant", "its depth"],
             ["Intonation", "Register", "the note", "Chiff", "Drift"],
             ["The Works", "Channel", "Division", "Transpose", "Blower" if PLATE else ""]],
            [0.24, 0.17, 0.19, 0.22, 0.18]),
      table([["Control", "Does"],
             [sc("fx sel env lfo edit glo arp"), "The stops, bass and treble"],
             [sc("rec"), "Cancel all stops / bring them back"],
             [sc("seq"), "Tremulant"],
             [sc("play"), "Blower"],
             [sc("save"), "Keep this setup"],
             [sc("home"), "The Console"],
             [sc("select"), "Turn the pages"],
             [sc("presets"), "Plate mix" if PLATE else "Room mix"],
             [sc("algorithm"), "Temperament"],
             [sc("master"), "Volume"]],
            [0.45, 0.55])]

if __name__ == "__main__":
    doc = Doc(OUT)
    doc.build(S)
    print(f"{OUT}: done")
