# Mantaflow reference notice

DVE v1.58 was informed by the user-supplied Mantaflow source archive. Mantaflow is an Apache-2.0
fluid-simulation research framework by the Mantaflow team.

DVE does not embed Mantaflow's Python scene system, generated-kernel preprocessor, Qt GUI, Blender
integration, bundled examples, or third-party build stack. The native DVE grid-fluid module is a
clean engine-facing implementation of standard MAC-grid, semi-Lagrangian/MacCormack advection,
pressure-projection, combustion, vorticity, and PIC/FLIP concepts. The upstream license is retained
in `third_party/notices/MANTAFLOW_LICENSE.txt` for attribution and auditability.
