# Save files: optics follow-ups from review (22 Oct 2026)

Two follow-ups to the material optics PR (#179): one from Codex review, one from Ben's merge note.

## Source-backed materials no longer repeat their optics

A voxel object whose material table matches its DVOX source is saved as "materials from source".
Loading restores the whole table, optics included, from that source.

The `dve.material_optics` section still listed every optical material of every such object. With
many copies of one glass asset, the section grew as copies × materials and could push a compact
save over its size limit. Now it lists only the objects whose material tables are written inline.

## Saves without optics stay at version 4

Version 5 differs from version 4 only by the optional optics section. A save with no optics is now
written as version 4, so builds from before optics can still open it. Saves that carry optics are
written as version 5. Loading is unchanged: version 4 goes through the existing no-op step to 5.

- `kGameSaveSchemaVersionWithoutOptics = 4` (`include/dve/game_save.hpp`).
- `SaveGameStore::write_atomic` gains `keepSchemaVersion`: it writes the document's own version
  (1 to current) instead of stamping the current one. `GameSaveCodec::write_file` uses it.
- `GameSaveCodec::from_document` also accepts a version 4 document, as long as it has no optics
  section.

## Tests (`tests/test_game_save.cpp`)

- A save without optics is version 4 in memory and on disk, and reads back through the one no-op
  step.
- A save with a glass material is version 5 with the section.
- Twenty copies of a glass DVOX: no optics section, and all twenty get their optics back from the
  source.
