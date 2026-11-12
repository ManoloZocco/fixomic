---
name: fixomic
description: Menu-bar extra that cleans the mic and publishes it as fixomic Mic; the panel is the voice's route.
colors:
  ground-light: "#f6f7f9"
  ground-dark: "#1b1e23"
  ink-light: "#1b1e23"
  ink-dark: "#eef0f3"
  ink-secondary-light: "#545b66"
  ink-secondary-dark: "#a4aab4"
  ink-tertiary-light: "#6a727e"
  ink-tertiary-dark: "#7f8692"
  hairline-light: "#e1e4e8"
  hairline-dark: "#2e3239"
  track-light: "#e1e4e8"
  track-dark: "#2e3239"
  switch-off-light: "#c9ced5"
  switch-off-dark: "#3b4048"
  control-light: "#ffffff"
  control-dark: "#2e3239"
  control-edge-light: "#c9ced5"
  control-edge-dark: "#3b4048"
  hover-light: "#eceef1"
  hover-dark: "#24282e"
  accent-light: "#2d6bdf"
  accent-dark: "#4a8cff"
  line-active-light: "#1e9a52"
  line-active-dark: "#3cc572"
  line-waiting-light: "#6a727e"
  line-waiting-dark: "#7f8692"
  line-bypass-light: "#b87900"
  line-bypass-dark: "#f0b43c"
  line-problem-light: "#d2342b"
  line-problem-dark: "#ff6259"
typography:
  title:
    fontFamily: "system-ui (SF Pro)"
    fontSize: "15pt"
    fontWeight: 600
  body:
    fontFamily: "system-ui (SF Pro)"
    fontSize: "13pt"
    fontWeight: 400
  body-strong:
    fontFamily: "system-ui (SF Pro)"
    fontSize: "13pt"
    fontWeight: 500
  caption:
    fontFamily: "system-ui (SF Pro)"
    fontSize: "11.5pt"
    fontWeight: 400
  micro:
    fontFamily: "system-ui (SF Pro)"
    fontSize: "10pt"
    fontWeight: 500
rounded:
  panel: "14pt"
  notice: "8pt"
  button: "6pt"
  pill: "10pt"
spacing:
  edge: "16pt"
  group: "12pt"
  row: "4pt"
components:
  switch-on:
    backgroundColor: "{colors.accent-light}"
    rounded: "{rounded.pill}"
    width: "34pt"
    height: "20pt"
  switch-off:
    backgroundColor: "{colors.switch-off-light}"
    rounded: "{rounded.pill}"
    width: "34pt"
    height: "20pt"
  button-push:
    backgroundColor: "{colors.control-light}"
    textColor: "{colors.ink-light}"
    typography: "{typography.caption}"
    rounded: "{rounded.button}"
    height: "24pt"
  button-flat:
    textColor: "{colors.ink-light}"
    typography: "{typography.body}"
    rounded: "{rounded.button}"
    height: "28pt"
  button-flat-hover:
    backgroundColor: "{colors.hover-light}"
  notice:
    textColor: "{colors.ink-light}"
    typography: "{typography.caption}"
    rounded: "{rounded.notice}"
    padding: "8pt 12pt"
---

# Design System: fixomic

Source of truth: `src/ui/Theme.{h,cpp}` (palettes, fonts, switch and buttons) and
`src/ui/MainPanel.{h,cpp}` (route strip, meters, notices, layout). Tokens above give the light
value; each `-dark` sibling is its dark counterpart. Rendered: `docs/screenshots/`.

## Overview

**Creative North Star: "The Voice's Route"**

fixomic lives in the menu bar and is opened for seconds: is my voice going out clean, and if not,
why. The panel answers like a metro route strip above a train door: one line, four stations
(Microfono, Eco, Catena, fixomic Mic), drawn in the ink of the current state. Filled stations
process the voice, hollow ones are skipped, a barred red one is where the problem is. Everything
else on the panel is a native macOS control in service of that line.

It is a working tool, not a mixer: the spec's anti-goal is "a DAW", so there are no cards, no
knobs, no meters with peak-hold LEDs. Type carries the hierarchy, hairlines separate groups, and
colour is spent only on state.

**Key Characteristics:**
- One state sentence, the largest text on the panel, in plain ink.
- The route strip is the one signature move; the level meters share its ink.
- Native switches and buttons; system font; follows light / dark.
- What global bypass skips (echo cancellation, chain) recedes to 45 % opacity but stays operable.

## Colors

Restrained, cool neutrals from a single stepped ramp per appearance, four state inks, one accent.

### Primary
- **Switch Blue** (#2d6bdf / dark #4a8cff): switches that are on, keyboard focus rings. Nothing else.

### State inks (the route line and the meters)
- **Line Green** (#1e9a52 / #3cc572): Running.
- **Line Slate** (#6a727e / #7f8692): Idle, Starting, waiting for a permission prompt.
- **Line Amber** (#b87900 / #f0b43c): global bypass; also the tint of warning notices (degraded).
- **Line Red** (#d2342b / #ff6259): MicMissing, DriverMissing, UnsupportedSampleRate, Error; a
  clipping meter; the closed station and its label; a plugin that could not be added (picker).

### Neutral
- **Cool Paper** (#f6f7f9) / **Night Platform** (#1b1e23): panel ground.
- **Ink** (#1b1e23 / #eef0f3): primary text. **Secondary Ink** (#545b66 / #a4aab4): labels,
  breakdown lines. **Tertiary Ink** (#6a727e / #7f8692): meter numerals, skipped stations (≥ 4.5:1).
- **Hairline / Track** (#e1e4e8 / #2e3239): separators and meter tracks.

### Named Rules
**The Reserved Ink Rule.** A state ink appears only where it encodes state: the route line, its
stations, the meter fill, a closed station's label, the warning tint. Never decoration.

**The One Ramp Rule.** Every grey is a step of the appearance's neutral ramp
(`kLightRamp` / `kDarkRamp` in Theme.cpp); no ad-hoc greys.

## Typography

**Font:** the macOS system UI font (SF Pro), through `juce::Font::getSystemUIFontName()`. One
family; weight and size make the steps.

### Hierarchy
- **Title** (600, 15 pt): the state sentence; wraps, up to four lines for long errors.
- **Body** (400, 13 pt): row labels (Ingresso, Cancellazione eco, Latenza), flat buttons.
- **Body Strong** (500, 13 pt): values (total latency) and the Catena heading.
- **Caption** (400, 11.5 pt): secondary lines, notices, push buttons.
- **Micro** (500, 10 pt): station names, meter numerals.

Italian decimal comma in every number ("30,1 ms"); a real minus sign (−40) on the meter scale; an
em dash "—" is the placeholder for a value not measured yet.

## Layout

360 pt wide, 16 pt edges, height follows the content (≈ 400–430 pt; notices add rows, each plugin
adds a 32 pt row, the plugin picker ≈ 650 pt). Order is
fixed by the spec: state sentence + bypass switch, route strip, notices, levels, echo
cancellation, chain, latency, footer (Impostazioni… left, Esci right, text aligned to the content
edges). Groups are separated by 12 pt, a 1 px hairline, 12 pt. Labels left, controls and values
right-aligned on one row. The panel opens centred under the menu-bar icon, 6 pt below the menu bar.
It is never taller than the display (8 pt from its bottom): a longer panel scrolls inside the
window with the trackpad, without a scrollbar, its rounded ground following the visible part.

## Elevation & Depth

Flat. The only depth is the window's native shadow (the panel is a borderless, non-opaque window
with a 14 pt rounded ground and a 1 px edge: black 12 % / white 14 %) and the switch knob's soft
shadow. No shadows inside the panel.

## Shapes

Rounded rectangles only: panel 14 pt, notices 8 pt, buttons 6 pt, switch and meter bars fully
rounded. Station markers are 11 pt circles with a 2 pt ring of ground where they sit on the line.

## Components

- **Route strip** (`RouteStripView`): 3 pt line between the first and last station in the state
  ink; InService = filled marker, Skipped = hollow 2 pt ring + tertiary label, Closed = red disc
  with a ground-coloured "no entry" bar + red label.
- **Level meter** (`LevelMeter`): 6 pt bar on −60…0 dBFS, ruled at −40 / −20 / −10 dB by notches in
  the ground colour; fill in the state ink while Running, red when ≥ −1 dBFS; empty otherwise. The
  numerals sit under the second meter only.
- **Switch** (`Theme::drawToggleButton`): 34 × 20 pt, accent when on, ramp step when off, white
  knob; focus ring in accent at 45 %; 40 % when disabled. The button text is its accessible name.
- **Push button**: control fill, 1 px edge, 6 pt radius, caption text; hover / pressed overlay ink
  at 5 % / 10 %.
- **Flat button** (`fixomic.style = flat`): text only; hover fill = hover step.
- **Notice** (`NoticeRow`): amber tint (13 % light / 16 % dark) at 8 pt radius, caption text, an
  "Apri Impostazioni" push button when a System Settings pane fixes it. More than two startup
  warnings merge into one row ("N avvisi all’avvio: …") elided with "…" at three lines.
- **Chain list** (`ChainList`): 32 pt rows in signal order, no boxes. Left to right: a drag handle
  (2 × 3 tertiary dots), the name (Body; secondary ink when bypassed, tertiary when missing; a click
  opens the editor, with the flat button's hover fill hugging the name), a caption at the right
  (the plugin's latency in ms when above 0, "In bypass", "Plugin non trovato"), a switch (on = the
  plugin processes; none for a missing plugin) and a × remove button (secondary ink, flat hover, no
  confirmation). Dragging dims the row to 35 % and marks the drop gap with a 2 pt line in ink that
  starts at a 7 pt ring under the handles; no line where the row would not move, nor away from the
  list (more than a row above or below, or past the panel's sides), where a release cancels; Esc
  cancels too. Under the rows
  the "+ Aggiungi plugin" flat button; with no plugins a caption line invites to add one first.
- **Plugin picker** (`PluginPicker`): replaces the Catena section in place (never a window). A
  header with a ‹ back button and "Aggiungi plugin" (Body Strong), "Cerca nuovi plugin" (flat) at
  the right; a search field with the push button's fill, edge and 6 pt radius, a tertiary
  magnifier and an accent focus ring; 6.5 rows of 28 pt (a cut row says it scrolls), the hovered /
  selected one on the hover step, the manufacturer · format in secondary caption at the right; one
  caption status line under it ("Scansione in corso…", or "Impossibile caricare «nome»." in Line
  Red; why it failed goes to the log, never to the panel). The list's
  place says "Nessun plugin trovato", or invites a scan when the catalog is empty. Esc or ‹ go back.
- **Menu-bar icon** (`TrayIcon::iconImage`): template image, 20 pt at 2x, 1.5 pt strokes: filled
  microphone (active), outline (waiting), outline crossed by a bar with cut-outs (bypass), outline
  with a knocked-out "!" badge (problem).

## Do's and Don'ts

- Do keep every control a standard macOS one (switch, button, menu).
- Do let the route strip and the meters carry state colour; keep text in ink.
- Do show "—" for a value not measured yet, never "nan" or 0.
- Don't add cards, boxed rows or coloured side stripes; groups are space and hairlines.
- Don't add mixer devices (faders, knobs, peak LEDs, dB readouts that flicker at 30 Hz).
- Don't use a state ink for anything that is not state.
