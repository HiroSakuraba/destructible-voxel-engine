# Editor legibility: toolbar, viewport help, camera preview, menus and inspector

Fixes the clipped and overlapping text in the main editor chrome (artist worklist items
ART-001 to ART-005, ART-008 and ART-120, and the menu half of ART-114).

## What changed

- **Toolbar (ART-001, ART-120).** Buttons were a fixed 50 px wide, so "Select", "Remove" and
  "Anchor" ran past them. The nine buttons now share the toolbar width (30 to 84 px each,
  leaving room for the mode label). A button shows the full tool name, then a short name
  ("Rem"), and never draws past its edge. The "1".."9" prefixes are gone: they were not
  shortcuts. Hovering a button shows a tooltip with the name, its real shortcut (for example
  Select Q, Move W, Rotate E, or "no shortcut"), what a click does, and what it acts on. Names,
  descriptions and the shortcut command live in one table, `editor_tool_info()`.
- **Viewport help (ART-003).** The single long hint line is replaced by the active tool's two
  or three gestures with their current bindings (for example "Drag axis to move  [/] snap
  step  Space world/local"). Only whole gestures that fit left of the camera preview are shown.
  The old hint also named the wrong keys ("T move  R rotate"; Move is W and Rotate is E). A
  "? Shortcuts" control in the viewport's bottom-right corner opens the shortcut guide.
- **Camera preview title (ART-004).** The rig name is elided before the "[Preview]" tag, the
  full name shows on hover, and the viewport hint stops before the preview.
- **Menus (ART-002, ART-114).** Each row reserves the shortcut column first (up to half the
  row) and elides the label before it. Hovering a row whose label was cut, or a command that
  is unavailable, shows a tooltip with the full label, shortcut and the reason it is
  unavailable (previously only the word "Unavailable").
- **Inspector (ART-005).** Details that do not fit above the flag toggles now scroll with the
  mouse wheel, with a scroll bar, instead of being cut off (at 800×600 the Prefab and Layer
  lines were hidden). Editable Position and Rotation fields move with the scroll and are not
  clickable when scrolled away. Text is clipped to the inspector instead of running past it.
  Rows do not reflow yet: long values are elided at the inspector edge.

## Validation

- New `dve_editor_chrome_fit_tests` (ART-008) renders the editor at 1280×720, 1536×960,
  1366×768, 1920×1080, 2560×1440, 800×600 and 640×480, each at 100 to 200 % UI zoom, and checks
  that every toolbar label stays inside its button, the viewport hint stays in the viewport and
  left of the camera preview, the help control is inside the viewport, and no menu row's label
  runs into its shortcut column. It also checks the toolbar tooltips and real shortcuts, the
  help control opening the guide, elision and tooltip of a long rig name, the reason tooltip on
  an unavailable menu command, and inspector scrolling at 800×600.
- Screenshots of the X11 editor under Xvfb at 1280×720 and 800×600 confirm the toolbar,
  tooltip, hint, help control and inspector scroll bar.
