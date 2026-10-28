# Exact CAD edge fillets: bounded first slice

The Polygon menu offers **Exact CAD Edge Fillet (Constant Radius)** and
**Exact CAD Edge Fillet (Variable Radius)**, and **Exact CAD Face Fillet** on one visible, unlocked exact CAD object.
They make a new object; the source remains intact. The variable command uses an
OpenCASCADE linear radius law between the two edge endpoints.

Type plain numbers in **mesh-local millimetres, before object scale**:

- Constant: `radius x1 y1 z1 x2 y2 z2`
- Variable: `start-radius end-radius x1 y1 z1 x2 y2 z2`
- Adjacent faces: `radius face-index-A face-index-B` (zero-based face indices)

Use the CAD Measure tool on a face to see its index. The face command requires two faces sharing exactly one open edge and applies a constant radius to that edge.

For example, a 100 mm cube centered on the origin can use
`5 -50 -50 -50 -50 50 -50`, or
`5 10 -50 -50 -50 -50 50 -50`. The first and second radii belong to the first and
second endpoints respectively, regardless of the underlying edge's orientation.
The points must identify one open B-rep edge to within 0.0001 mm. A missing or
ambiguous edge is refused; a closed edge or tangent-connected multiple-edge contour
is not supported. Curved edges may be selected only when their two endpoints identify
one edge uniquely. The numeric input does not yet provide graphical edge picking.

The input and output must each be one valid OpenCASCADE solid. An OCP Python
interpreter is required; set `DVE_CAD_PYTHON` if the Python on PATH does not have
OCP. The resulting B-rep is imported and meshed through brepkernel, but this operation
is **not a brepkernel-certified Boolean**. OCCT validity and passing an import do not
constitute a dimensional or global self-intersection certificate. The operation fails
without changing the scene if construction, validation, import or the source-stamp check
fails. A fillet may create freeform surfaces, including at varying radii. Check the
result with the exact CAD inspection tools before manufacturing.

The new object preserves pose, parent and object scale, and receives a fresh exact
B-rep source. Its per-face material assignments must be reapplied. A tube record is
not copied to the filleted output: the previous stock description would no longer
fully describe the object. Nonuniform scaling can change a circular local fillet's
world-space shape; use uniform scale for a circular world-space radius.

This first slice does not implement non-adjacent face blends,
full-round fillets, radius controls at arbitrary positions, graphical edge selection,
fillet feature history or re-editing after source changes. Those remain separate work.
