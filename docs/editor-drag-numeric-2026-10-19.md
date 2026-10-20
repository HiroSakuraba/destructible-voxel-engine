# Typed values during a transform drag (19 Oct 2026)

Artist worklist ART-029. Builds on the real scale branch.

While dragging a Move, Rotate or Scale gizmo handle, just start typing:

- **Numbers** set the amount along the grabbed axis: meters for Move, degrees for Rotate, a factor
  for Scale. `-` works first, `.` once, Backspace edits.
- **Units** follow the number: `25cm`, `3mm`, `1.5ft`, `6in`, `2km` for Move; `0.5rad`,
  `0.25turn`, `90deg` for Rotate. Scale takes plain factors.
- **X, Y or Z** switch the axis the value applies to (from the uniform Scale handle too).
  Rotate starts measuring around the new axis from that moment.
- The value shows by the cursor, for example `Move X: 25cm = 0.25 m (X/Y/Z axis, Enter applies,
  Esc cancels)`.
- **Enter** commits one undo step; **Esc** puts everything back.

While a value is being typed, other letter keys are ignored, so a stray key cannot switch tools in
the middle of a drag.

Typing digits during a drag was already possible (plain numbers, no units, no feedback on
screen); this adds units, axis switching, the on-screen value and the key guard.

## Tests

`tests/test_editor_drag_numeric.cpp`: Move with cm, ft and plain meters, the drawn label, axis
switching, Backspace, Enter as one undo step, Esc; Rotate in degrees and turns; Scale with a typed
factor, axis switching, and units refused for a factor.
