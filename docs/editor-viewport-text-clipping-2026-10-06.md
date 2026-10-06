# Viewport text clipping

Follow-up to PR #48, which clipped the 3D viewport's fills and lines to the viewport rect.

## The gap

`RectClipCanvas` dropped text only when its anchor (x and baseline) lay outside the clip.
A label anchored inside could still draw past the edge: the 3D-text name and
"Slug M<face>/<side>" labels and the Gabor "Gabor <count> L<lod>" labels of an object
partly off screen ran past the right edge onto the inspector, and a label whose baseline
sat just inside the top drew its glyphs above the viewport. The #48 test did not see it
because its raster canvas ignored text.

## The fix

- Text that would run past the right edge is elided to fit with the canvas's ellipsis
  (`elide_text_to_width`, as table cells already do), and dropped when not even the
  ellipsis fits.
- Text whose glyphs would cross the top or bottom is dropped (glyphs cannot be cut). The
  glyph extent comes from new constants `kEditorTextAscent` (10) and `kEditorTextDescent`
  (3), logical pixels for the 11 px UI font. Every fixed viewport label (HUD, overlays,
  camera preview title) sits at least 12 px from the top and bottom, so none is affected.
- `RectClipCanvas` moved from the renderer's anonymous namespace into
  `dve/editor_native_renderer.hpp` so it can be tested directly.

## Validation

`dve_editor_viewport_clipping_tests`:

- new `text clipping` case on `RectClipCanvas`: text that fits is unchanged; long labels at
  the right edge are elided to fit or dropped; text touching the top and bottom from inside
  is kept and one pixel further is dropped; text anchored left or right of the clip is dropped;
- the frame tests now rasterize text as the box its glyphs can cover, so the orbit and
  48-pose sweep also check that no viewport text lands outside the viewport.

Full build (Release, SDL3 unavailable): 251 of 252 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
