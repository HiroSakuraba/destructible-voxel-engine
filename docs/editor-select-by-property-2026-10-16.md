# Select by property (16 Oct 2026)

Artist worklist ART-019. Builds on the selection filters branch (masks).

## Queries in the hierarchy filter

The hierarchy filter still matches names when you type plain text. As soon as the text has a
`key:value` term or a `-` negation, it becomes a query:

| Term                    | Matches                                                         |
|-------------------------|-----------------------------------------------------------------|
| `wall`                  | name contains "wall" (every bare word must match)               |
| `type:mesh`             | `voxels`, `mesh`, `subdivision`, `text`, `gabor`, `helper`, `group` |
| `tag:rock,cliff`        | any listed tag (commas mean "or")                               |
| `layer:2`               | that layer                                                      |
| `material:3`, `material:stone` | a voxel palette id, or a named voxel or polygon material |
| `prefab:tree`           | instances whose prefab asset path contains "tree"               |
| `component:spawn`       | objects with a component whose type contains "spawn"            |
| `parent:rig`            | objects with an ancestor whose name contains "rig"              |
| `locked:yes`, `visible:no`, `anchored:yes` | flags                                        |
| `-tag:rock`             | a leading minus negates any term                                |

Terms combine with "and"; keys and values ignore case. A misspelt key shows an error in the
status line rather than silently matching nothing. The hierarchy keeps showing the parents of
matches, as before.

The query engine is `include/dve/editor_selection_query.hpp` (no editor state, so it can be reused
by named selection sets and tools).

## Select Matching Filter

Edit > Select Matching Filter selects every object the current hierarchy filter matches, names or
query, switching to the object domain if needed. It selects exactly what the filter says, hidden
or locked objects included, since the query can ask for those itself.

## Select Similar

Edit > Select Similar: Same Type / Same Material / Same Tag / Same Layer / Same Prefab adds every
visible, selectable object (the selection masks apply) that shares that property with any
selected object. Materials compare voxel palette ids and material names.

## Saved queries

Saving queries arrives with named selection sets (ART-021, next branch): a set can store a query
and is re-evaluated each time it is recalled.

## Tests

`tests/test_editor_select_by_property.cpp`: every query key, negation, "or" values, "and" terms,
error messages; the hierarchy filter with queries; Select Matching (query, plain name, bad
query); each Select Similar command, including masks and hidden objects.
