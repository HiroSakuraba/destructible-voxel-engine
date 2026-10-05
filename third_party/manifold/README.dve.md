# manifold (vendored for DVE print export)

- Upstream: https://github.com/elalish/manifold, tag **v3.5.1** (Apache 2.0)
- Pruned for the engine: only `src/`, `include/`, `cmake/`, `CMakeLists.txt`,
  and `LICENSE` are kept (plus `bindings/CMakeLists.txt`, which only adds subdirectories when bindings are enabled). Dropped: `samples/`, `test/`, `bindings/`, `docs/`,
  `extras/`, and the top-level Python/JS scaffolding — none are needed to build
  the C++ `manifold` library target the engine links.
- The engine configures it with `MANIFOLD_TEST/PAR/CROSS_SECTION/CBIND/PYBIND`
  forced OFF and adds it `EXCLUDE_FROM_ALL`.
- To refresh: clone the tag, re-prune the same five entries, and update the tag
  name above.
