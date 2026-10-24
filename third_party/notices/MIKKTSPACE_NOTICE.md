# MikkTSpace (vendored, altered)

`third_party/mikktspace/mikktspace.c` and `mikktspace.h` are the MikkTSpace tangent-space
reference implementation by Morten S. Mikkelsen, vendored for the polygon cooker
(`src/polygon_tangents.cpp`). License: zlib (text below, copied from the header of
`mikktspace.c`). The file is compiled as C++ (see `CMakeLists.txt`).

## DVE changes to the vendored source (zlib clause 2: altered source versions are marked)

`mikktspace.c`, `QuickSort()` and `QuickSortEdges()`: the pseudo-random pivot rotate

    t=uSeed&31;
    t=(uSeed<<t)|(uSeed>>(32-t));

shifts a 32-bit `unsigned int` right by 32 when `t == 0`, which is undefined behaviour
(UBSan: "shift exponent 32 is too large for 32-bit type 'unsigned int'", reported by the
linux-gcc-asan CI leg). It is now

    t=t?((uSeed<<t)|(uSeed>>(32-t))):uSeed;

A rotate by 0 is the identity, which is what x86 produced for the masked shift, so the
sort pivots, and therefore the generated tangents, are unchanged. A header comment in the
file marks it as altered. The fix appears twice (once per function); no other change.

## License (zlib)

Copyright (C) 2011 by Morten S. Mikkelsen

This software is provided 'as-is', without any express or implied warranty. In no event
will the authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to the
following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you
   wrote the original software. If you use this software in a product, an acknowledgment
   in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented
   as being the original software.
3. This notice may not be removed or altered from any source distribution.
