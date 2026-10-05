# DASHR research attribution

DVE's DASHR surface-space heightfield foundation is informed by the algorithms and
reference implementation accompanying:

Tom Forsyth. **DASHR — Dynamically Animated Skinned Heightfield Rendering.**
Version 1.0, 27 September 2026.

Upstream source:

https://github.com/tomforsyth1000/DASHR

The upstream paper states that the software is released under the most permissive
licenses possible and allows users to choose either the MIT No Attribution License
(MIT-0) or a public-domain/Unlicense-style alternative. DVE records and uses the
MIT-0 option. The MIT-0 text is retained in `third_party/DASHR-LICENSE.txt`.

DVE does not vendor the upstream DirectX 11 application, executable, ImGui/demo
framework, third-party libraries, or demo asset corpus. The engine implementation is
a native C++23/HLSL port of the transferable surface-space mapping, distortion
damping, seam teleport, and heightfield ray-marching concepts.
