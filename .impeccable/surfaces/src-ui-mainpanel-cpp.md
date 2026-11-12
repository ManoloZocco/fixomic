---
version: 1
slug: "src-ui-mainpanel-cpp"
primary_target: "src/ui/MainPanel.cpp"
related_targets: ["src/ui/TrayIcon.cpp","src/ui/Theme.cpp"]
---

# Menu-bar panel (fixomic)

Scope: the menu-bar extra of fixomic: the status icon (four states) and the compact panel that
opens under it. Mode: Operate. Native macOS (JUCE LookAndFeel_V4), ~360 pt wide, light and dark
following the system. Built unattended: no interview round was possible; every choice below is
the builder's, from the spec's "Interfaccia" section and PRODUCT.md.

Audience / task: someone about to join, or already in, a call wants to know in one glance whether
their voice reaches fixomic Mic clean, and if not, why and what to do. Toggles: global bypass,
echo cancellation. Reads: levels in / out, echo removed, total latency. Later (10b/10c): the
plugin chain, settings, onboarding.

Constraints: rows in the spec's order (status + bypass; levels; AEC + echo removed; chain;
latency; settings). Decided Italian status strings. Anti-goal: a DAW. Controls stay the
platform's standard ones (switches, buttons). Code-led build (Operate surface, no image tool).

## Direction contract

THESIS: the panel is the signal's route, not a mixer. It refuses the category default of a
stack of grey cards with sliders; it states one thing big (the state) and shows the path the
voice travels as a single transit line.

OWN-WORLD: transit-line wayfinding (metro route strips above train doors) translated to a
native macOS panel: one cool-tinted neutral ground per appearance, a single stepped neutral ramp
for every grey, four state inks (active green, waiting slate, bypass amber, problem red) used only
for state, one system-blue accent only on switches that are on. System font (SF), one family,
three sizes. No cards, no borders around rows: groups separated by space and hairlines.

STORY: the visitor reads the state sentence, sees which stations the voice passes through
(filled = in service, hollow = skipped, red ring = closed), checks the levels moving, and flips
bypass or echo cancellation if needed. Problems name their fix and offer the matching System
Settings pane.

FIRST VIEWPORT: the whole panel is the first viewport. Top: state sentence (15 pt semibold,
wraps) with the bypass switch at its right; under it, full width, the route strip Microfono —
Eco — Catena — fixomic Mic in the state's ink. Then notices (tinted, only when present), levels
(Ingresso / Uscita bars on a dB scale), Cancellazione eco switch with "Eco rimossa" value, Catena
section (empty state now), Latenza total with its breakdown line, footer Impostazioni… / Esci.

FORM: transit diagram (midnight-transit challenger) fused with the product's own signal path;
own list position: challenger, beat assigned #4 (audiogram) on audience identification and product
clarity. Raises kept: stepped neutral ramp (exposure record), one reserved ink per state
(orienteering course purple), skipped stages recede under bypass (streaming wall), chrome deleted
and type carries hierarchy (metro tiles), dB-ruled meter scale (audiogram). seed:f181a3e24fce2742

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
