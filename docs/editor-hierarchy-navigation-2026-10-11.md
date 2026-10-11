# Hierarchy navigation (ART-037, ART-122)

The scrolling hierarchy previously had no way to fold a branch, reveal an offscreen selection,
retain parent context while filtering, or navigate rows with the keyboard.

- Click a parent's `>` / `v` disclosure to collapse / expand it. A disclosure never selects an
  object or starts a reparent drag. Folding preserves the current selection, even if it is in
  the folded branch. This is a view operation; it does not close an authored group, affect
  viewport picking, dirty the scene or add an undo step.
- **View > Expand All Hierarchy Rows / Collapse All Hierarchy Rows** are also in Command Center.
- Name filtering is case insensitive and includes the complete ancestor path of every match.
  It temporarily shows matching paths regardless of folding. Clear the filter to recover the
  folded view; changing the filter resets its scroll to the top. Collapse/expand is refused
  while filtering, with a reason, so a match cannot disappear behind a folded parent.
- **View > Reveal Selection in Hierarchy** (also in the object context menu / Command Center)
  clears the filter, expands the primary selection's ancestors, opens the hierarchy panel,
  focuses it and scrolls the selected row into view. It keeps the primary and multiselection.
  Other branches retain their folded state. With no selection, the command explains the
  prerequisite.
- With hierarchy focus, **Up / Down** select the previous / next displayed row without wrapping.
  **Home / End** select the first / last displayed row. **Left** folds an expanded parent, or
  selects its parent. **Right** expands a folded parent, or selects its first displayed child.
  The list scrolls with keyboard selection. Text edits, menus and modal panels retain their
  existing key handling. Alt+arrows in the viewport still perform hierarchy walking.
- New Scene, New Project and Open Scene clear folds, filter and scroll because object ids can be
  reused by another scene.

Filtering uses forward/reverse hierarchy passes and ordered sets; it does not scan every
ancestor chain for every match. Disclosure hit rectangles share the displayed-row indices and
are empty for leaves and offscreen rows. Visual indentation retains the existing eight-level
cap; filtered paths still list all their ancestors.

## Remaining work

ART-037 remains partial: folding is session view state, not persisted per scene across restart.
There is no separate breadcrumb bar or automatic reveal on every viewport pick. Use the explicit
Reveal command. ART-122 remains partial: inspector and toolbar keyboard routes need work.
Closed/open authored groups, inherited hide/lock, group bounds and pivots are separate tasks.
Artist acceptance is still needed, particularly for deep scenes, filtering and two-handed
keyboard/mouse workflows.

## Validation

`dve_editor_hierarchy_navigation_tests` covers nested folds, selection and undo preservation,
repeated disclosure clicks, rendered disclosure indicators, leaf/missing ids, case-insensitive
filtering across branches, ancestor retention, filter refusal/clear behavior, visible-row
keyboard navigation and bounds, multiselection-preserving reveal, a hidden hierarchy panel,
scrolling in a 100-object scene and state reset on new/open scene.

Local validation: a Release build passed with SDL/native hosts disabled. Six focused tests passed:
hierarchy navigation, voxel selection, honest commands, honest follow-ups, chrome fitting and
layout overlap. These exercise the controller and recording canvas; native window interaction
and artist acceptance were not performed. `git diff --check` and source manifest freshness were
also checked before publishing.
