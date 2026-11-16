# Machine avatar presets

Pirwa ships 16 original minimal robot expressions. Each uses the same 64 × 40
screen grid and simple rounded eyes and mouths. While the machine is running,
its preset's colored background fills the monitor's full screen, with dark
features for contrast. The fill extends under the bezel so it has no inset gap.
The monitor frame follows the normal GTK foreground color in every state;
the whole icon is never tinted with the accent color. Paused/stopped machines
and picker previews use transparent screens and normal foreground features.
One shared screen mask uses the colors recorded in `src/pirwa-avatar.c`.
Feature strokes are stored as filled outlines so GTK's symbolic SVG renderer
preserves the curved eyes and mouths when recoloring them.

All assets are original Pirwa SVG artwork under GPL-3.0-or-later. The direction
uses expressive eyes and controlled variation, inspired by the general approach
described in [Designing Grok Bot](https://x.ai/news/designing-grok-bot); no Grok
artwork, fonts or code are included.

Friendly is the default (`generic` remains its stable preset identifier).
Old distro preset identifiers and unrecognized identifiers display Friendly;
the saved preference is untouched until the user explicitly saves a selection.
Expressions are personal identity choices. Their shapes stay the same across
VM power states; the screen fill appears only while running.

All SVGs are embedded in Pirwa. They use no animation, timers, external images,
font files, network requests or ISO inspection. To add a preset, add its SVG,
its catalog entry in `src/pirwa-avatar.c`, and its resource entry in
`data/pirwa.gresource.xml`.
