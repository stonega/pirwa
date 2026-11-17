# Pirwa app icon

The icon shows sunglasses and a small smile on a mint screen inside a single
blue monitor. The expression follows the supplied monitor-face reference.
The casing uses small corner radii, a flat bezel and a shallow darker lower
profile. A straight support and compact foot complete the simple silhouette.
The face uses the original dark violet pigment.

The sunglasses share one straight top bar, with two broad lenses and a short,
slightly asymmetric smile underneath. The symbolic version keeps the single
monitor and uses features adjusted for its 16 × 16 canvas.

The design follows the [GNOME app icon guidance](https://developer.gnome.org/hig/guidelines/app-icons.html):
a 128 × 128 canvas, simple geometry, flat surfaces, a darker lower profile,
and no shadow outside the silhouette. Its bottom edge sits at y = 120.
The full-color casing has a 6-unit corner radius and the screen a 2-unit radius.
The 16 × 16 symbolic version uses a 1-unit outer corner radius.

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

## Original concept and reference revision

The initial concept used the built-in image generation tool with transparency.
The original vector artwork simplified that concept into flat, scalable geometry.
The exact generation prompt is in [pirwa-icon-prompt.txt](pirwa-icon-prompt.txt).

The reference revision edits the native SVG directly, retaining the blue,
mint and dark violet palette. It replaces the upright eyes and broad smile
with sunglasses and a smaller smile, removes the rear display and extra glass
rim, and reduces the casing's corner radius from 14 to 6 units. The PNG export
and size/background preview are rendered from the updated SVGs with librsvg.

Validation covers SVG parsing, rendering at 16, 32, 64, 128 and 512 pixels,
transparent padding, and visual inspection on light and dark backgrounds.
