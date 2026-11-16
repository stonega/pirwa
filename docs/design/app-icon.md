# Pirwa app icon

The icon shows a friendly robot face on a mint screen inside a blue monitor.
A second, offset violet display suggests a collection of machines. The blue
and violet casing, shaded lower profile, stand and silhouette preserve the
original icon's material and depth. A pale blue glass rim surrounds the mint
screen, and the face uses the original dark violet pigment.

The face replaces the small nested guest computer so that the eyes and smile
remain readable at launcher sizes. Its simple construction matches the machine
avatar expressions. The symbolic version keeps the layered-monitor silhouette
and uses larger features adjusted for its 16 × 16 canvas.

The design follows the [GNOME app icon guidance](https://developer.gnome.org/hig/guidelines/app-icons.html):
a 128 × 128 canvas, simple geometry, flat surfaces, a darker lower profile,
and no shadow outside the silhouette. Its bottom edge sits at y = 120.
The 16 × 16 symbolic version keeps the same layered-display metaphor.

Assets:

- [Scalable full-color icon](../../data/io.github.stonega.Pirwa.svg)
- [Transparent 512 × 512 export](../../data/io.github.stonega.Pirwa.png)
- [Symbolic icon](../../data/io.github.stonega.Pirwa-symbolic.svg)
- [Size and background preview](pirwa-icon-preview.png)
- [Generated concept](pirwa-icon-concept.png)

The desktop entry uses `io.github.stonega.Pirwa`. Meson installs the full-color
SVG into `hicolor/scalable/apps` and the symbolic SVG into `hicolor/symbolic/apps`.
Both SVGs are also embedded in the application resource bundle, so running from
the build directory uses the same artwork. The RPM includes both installed icons.
The SVGs are editable vector artwork, with no embedded bitmap, fonts, filters,
or external resources. The PNG export and preview are rendered from the SVG.

## Original concept and face revision

The initial concept used the built-in image generation tool with transparency.
The original vector artwork simplified that concept into flat, scalable geometry.
The exact generation prompt is in [pirwa-icon-prompt.txt](pirwa-icon-prompt.txt).

The robot-face revision edits the native SVG directly and preserves the
original casing geometry and palette. The 512-pixel alpha silhouette matches
the previous SVG rendering exactly. A pixel comparison confirms that all
changes are contained within the original front screen. The PNG export and
size/background preview are rendered from the updated SVGs with librsvg.

Validation covers SVG parsing, rendering at 16, 32, 64, 128 and 512 pixels,
transparent padding, and visual inspection on light and dark backgrounds.
