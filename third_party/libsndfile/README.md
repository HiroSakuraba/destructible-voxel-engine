# libsndfile build inputs (DVE_FETCH_SNDFILE)

`cmake/DveSndFile.cmake` builds libsndfile 1.2.2 from
`https://github.com/libsndfile/libsndfile/archive/refs/tags/1.2.2.tar.gz`
(SHA256 `ffe12ef8add3eaca876f04087734e6e8e029350082f3251f565fa9da55b52121`). That file is
byte-identical to Debian's `libsndfile_1.2.2.orig.tar.gz`, which is also on snapshot.debian.org
(`https://snapshot.debian.org/file/28acc1c19b06c18f38f906d7efef404b2078a19a`).

`patches/` is Debian's patch series for `libsndfile 1.2.2-2+deb13u1` (from
`libsndfile_1.2.2-2+deb13u1.debian.tar.xz`, SHA256
`2549d7a791ca177213918fbcf9c9e39c5734a2e18782341b44d71e4b39365296`), unchanged. It carries the
security fixes that upstream has not released yet: CVE-2022-33065 (integer overflows),
CVE-2024-50612 (Ogg Vorbis error checking) and CVE-2025-56226 (MP3 encoder; not built here).
`series` lists the patches in the order they are applied. The patches are LGPL-2.1-or-later,
like libsndfile.

The upstream tarball, these patches and the CMake options in `cmake/DveSndFile.cmake` together
are the corresponding source of the `libsndfile.so.1` that DVE bundles.
