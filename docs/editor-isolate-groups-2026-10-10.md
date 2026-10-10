# Editor isolation and group labels

Artist worklist items ART-020 (isolate part) and ART-034, with a box-selection fix found
along the way. Builds on the snapping and Scale tool change.

## What changed

- **Isolate Selection (ART-020).** View > Isolate Selection shows only the selected objects
  and everything parented under them; choosing it again shows everything. Isolation is a
  view setting: it never changes an object's authored Visible flag, does not mark the scene
  modified, and is not saved. While isolated, the viewport draws, picks, box-selects and frames
  ("Frame All") only the isolated objects. The viewport says "ISOLATED: N object(s)  View >
  Isolate Selection shows all" so a filtered view is never mistaken for missing objects, and
  isolated-out rows are dimmed in the hierarchy. The command is a checkable menu item. Frame
  Selected and Frame All already existed.
- **Groups versus named groups (ART-034).** The hierarchy now indents children under their
  parent (12 px per level) and shows a parent's child count, for example "Group  (2)". The
  inspector shows "Parent group  <name>" (or "none") for unattached objects, and the
  membership count is now "Named groups" instead of "Groups", which read like the parent group.
- **Box selection ignored visibility.** Box selection selected hidden objects too. It now
  takes only visible objects (and, while isolated, only isolated ones).
- Hierarchy labels are elided at the row edge instead of running past the panel.

## Validation

- New `dve_editor_isolate_groups_tests`: isolating nothing is refused; isolating one object
  draws only it, keeps every authored Visible flag and the document's modified state, shows
  the menu item checked and the viewport banner, and box selection over the whole viewport
  takes only that object; toggling back draws everything again. Isolating a group keeps its
  children. The inspector names the parent group, uses "Named groups", and the child row is
  indented under its group with a child count.
- Screenshot of the X11 editor at 1280×720 after grouping two objects and isolating them:
  indented rows, "Group  (2)", "Parent group" and "Named groups" lines, and the banner.
