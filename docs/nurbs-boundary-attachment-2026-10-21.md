# Boundary matching with automatic NURBS compatibility

The native C0/C1 commands now match any U-min, U-max, V-min or V-max boundary of the other selected surface to any boundary of the active reference. Choose both boundaries in the NURBS menu, optionally reverse boundary correspondence (Twist), then run Match Other Boundary to Active (C0 or C1). Defaults remain active U-max and other U-min. The settings panel and status message show the current choices. These are editor-session options; choosing them does not change the document or add undo history.

Both source objects are retained. The result is one independent editable NURBS patch, with the moving object's appearance and the active object's coordinate frame/parent. The existing transformed-source path handles relative pose and nonuniform scale. Result creation, validation and tessellation finish before one AddObjectCommand publishes it. Undo/redo restores the generated object and selection. Source and derived cache remain covered by scene validation and save/reopen.

## Geometry contract

`attach_nurbs_boundaries` returns a shape-preserving compatible reference and an aligned moving surface. Native authoring publishes the aligned moving surface; the original reference remains unchanged because compatibility preparation does not change its geometric surface. The strict `attach_nurbs_u` API remains available with its original requirements.

1. Validate both clamped rational sources and both boundary choices.
2. Normalize parameter domains to [0,1] and temporarily transpose/reverse each patch so the reference seam is U-max and moving seam U-min. The optional Twist reverses the moving tangential direction.
3. Elevate the tangential degrees to their maximum. Refine both axes to the exact union of knot values and maximum multiplicities. There is no sampled fit, knot removal or near-knot snapping. Existing degree elevation can retain extra full-multiplicity span boundaries; no minimal-knot representation is promised.
4. Copy the reference's rational boundary representation for C0. For C1 also continue its homogeneous cross-boundary derivative into the moving patch. Cross derivatives use unit cross-boundary domains: outward from the reference equals inward into the moving patch. This is an explicit chart convention, not equality of raw derivatives measured in unrelated original U/V units. Tangential correspondence is affine in edge fractions, with reversal when selected.
5. A two-control linear moving cross-axis is elevated to quadratic for C1, providing an adjacent tangent row while preserving the opposite boundary. Common weight scaling is normalized; nonpositive, ill-conditioned or unbounded output weights/positions refuse the operation.
6. Undo temporary orientation and restore each input's parameter domains and texture ranges. The reference retains its geometry and derivatives within floating-point arithmetic; the moving patch's opposite boundary remains unchanged geometrically.

Compatibility obeys existing 4096-control, degree-seven and display/work budgets. Distinct knot values are kept distinct, even when close. Parameter normalization or restoration that loses representable spans is refused instead of silently merging knots. A geometrically folded or singular aligned patch may fail final display tessellation; native authoring then publishes nothing and adds no history.

## Autodesk references and scope

[Maya Align Surfaces Options](https://help.autodesk.com/cloudhelp/2027/ENU/Maya-Modeling/files/GUID-6A16F81C-AEB1-485C-BFD2-A32915F68D45.htm) documents positional/tangent/curvature continuity, choice of modified surface/boundary, Keep Originals, Reverse/Swap and Twist. This round implements fixed-reference boundary matching, kept inputs and explicit direction choices. It does not implement modification of both inputs, curvature matching, tangent-scale sliders or live construction history.

[Softimage 2014 Blend Surfaces](https://download.autodesk.com/global/docs/Softimage2014/en_us/userguide/files/property6004.htm) creates a surface across a gap and exposes C0/C1/C2, tangent scale and lateral direction matching. DVE's boundary matcher reshapes the moving patch; it does not create that blend strip. A blend generator remains later work.

There is no persistent adjacency, topological sewing, shared seam sampling, trim support or automatic crack-free tessellation in this batch. The next geometry batch should store boundary relationships and synchronize samples between related patches. No source codec change is required.

## Validation

16 selected local CTest suites passed: attachment, background, adaptive display, modeling, length, degree, boundaries/join, display, topology, construction, control tools, NURBS core/editor, native polygon, subdivision, scene export and editor chrome fit.

The dedicated attachment suite independently evaluates all 16 boundary pairings with both direction choices and C0/C1 policies. It checks seam positions, normalized cross derivatives, prepared-reference shape/derivatives, far-edge preservation, original domains/UV ranges, exact knot multiplicities, distinct nearby knots, extreme common weight scales and atomic argument/budget refusals. Native tests cover radio/toggle settings, transformed/scaled sources, retained inputs/materials, generated cache validation, selection/history, locked-input refusal and save/reopen. Headless checks do not establish windowed/GPU artist acceptance.
