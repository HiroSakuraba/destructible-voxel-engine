# BS-Cloth research attribution

The optional DVE B-spline cloth foundation was informed by the algorithms and
reference implementation accompanying:

Yuqi Meng, Yihao Shi, Kemeng Huang, Zixuan Lu, Ning Guo, Taku Komura,
Yin Yang, and Minchen Li. **Efficient B-Spline Finite Elements for Cloth
Simulation.** ACM Transactions on Graphics 45(4), Article 102, SIGGRAPH 2026.
DOI: 10.1145/3811278.

Upstream source: `Simulation-Intelligence/BS-Cloth`.

The upstream repository is licensed under the Apache License 2.0. Its license
text is retained in `third_party/BS-Cloth-LICENSE.txt`.

DVE does not embed the upstream application, preview renderer, YAML workflow,
prebuilt libraries, IPC/Newton solver, or asset corpus. The DVE implementation
is a native, dependency-free adaptation of the transferable surface,
quadrature, mass, seam, and bending-stencil concepts.
