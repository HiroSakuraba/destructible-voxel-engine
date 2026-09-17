# Lua 5.4 ABI fallback

This directory contains only the public Lua 5.4 declarations used by DVE's optional
`GameScriptHost`. It is not a Lua implementation. CMake uses it only when a system provides a
Lua 5.4 runtime library but not the development headers. Platforms with normal Lua development
packages use those complete headers instead.

The declarations follow Lua's public C ABI and MIT license. Lua itself is not redistributed in
this source bundle.
