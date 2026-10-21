# Curve splitting by world-length fraction

**NURBS > Split Curve at Length Fraction (Keep Source)** uses the shared
**Parameter / length fraction (0..1)** field in Construction Settings. Set 0.25
to put approximately one quarter of the selected curve's world length in the
first piece. Existing parameter-based split/isocurve/knot commands continue
to interpret this field as a normalized parameter. Length splits accept only
interior fractions; 0 and 1 do not create empty pieces.

The native action measures the rational source using the object's diagonal
scale and solves for its U parameter, with absolute tolerance 1e-6 in editor
world units. Rotation/translation do not affect length. It then reuses the
source-preserving knot-refinement split, retaining both editable pieces and
the original. Both results are prepared and baked before scene mutation and
enter one compound undo command. Placement, display settings, continuous
parameter-based texture ranges, materials and resource bindings follow the
existing split behavior. Undo/redo restores source selection and both results.
Texture allocation follows the parameter cut; it is not reallocated by length.

`nurbs_parameter_at_length_fraction` exposes the inverse query independently
of scene edits. The result includes the parameter, measured prefix/total length,
estimated residual and combined derivative-evaluation count. It first measures
total length, then bisects a bracket for at most 64 iterations. Every integration
shares one 200,000-derivative-evaluation budget and retains the per-seed depth
limit of 20. Acceptance combines the prefix residual, quadrature error estimates
and a floating-point rounding allowance. Invalid fractions, surfaces, zero
metric length, nonfinite values, exhausted budgets and unrepresentable accuracy
return no result. Fractions 0/1 return the domain endpoints for nonzero curves.
If a curve has a constant interior interval, any parameter meeting the tolerance
is acceptable; an earliest-parameter convention is not promised.

These are numerical estimates, not certified geometric error bounds. Localized
features can escape quadrature sampling. The inverse does not improve that
guarantee. The split preserves the rational curve's shape within floating-point
arithmetic; the location along that shape is numerically estimated. No scene
codec change is needed. Closest points, equal-distance point generation, rail
reparameterization, surface area, viewport annotations and live history remain
separate work.

Regression coverage adds rational weighted-line inversion, rational arc locations,
corners, nonuniform scale, stationary tangents, endpoint queries, common weight
scales, refinement/elevation/reversal invariance, failure diagnostics and native
split source/cache/material/placement/UV/history/selection/guard checks. Artist
acceptance in a window and GPU testing remain outstanding.

This batch is stacked on open curve-length PR #165, itself based on #164.
