# Native NURBS curve length

The NURBS menu adds **Measure Curve Length (World Units)** for one visible,
unlocked, non-prefab native curve. It integrates analytic source speed after
applying object scale, including nonuniform and reflected scales. Rigid rotation
and translation do not change length. The number uses editor world units, with
no extra voxel-size conversion. Surfaces must first supply an extracted curve.

The result appears in the status line for 15 seconds, including the estimated
numerical error. This read-only action preserves source, display cache, control
selection and command history. Stale source/cache pairs are refused.

`measure_nurbs_curve` in `nurbs_queries.hpp` also supports an ordered subinterval,
an absolute tolerance and a diagonal metric. It validates inputs, splits at knot
spans, seeds eight subintervals per span and uses adaptive Simpson integration
of the scaled analytic first derivative. Knot endpoints use one-sided interior
samples. The total estimated error is allocated across seed intervals. Work is
limited to 200,000 derivative evaluations and 20 recursive subdivisions per seed.
Exhaustion, unrepresentable subdivision and nonfinite arithmetic return no result.
No partial length is presented as a successful measurement.

The Simpson error estimate is **not a certified bound**. Very localized features
and ill-conditioned parameterizations can evade sampling; no global interval
certificate or relative-error promise is made. This does not provide surface
area, closest points, arc-length inversion, equal-distance resampling, viewport
measurement annotations or automatic rail reparameterization. Display density
does not determine the measured length. No scene codec change is needed.

Validation covers exact lines and corners, subintervals, rational circles,
refinement/elevation/reversal invariance, extreme common weight scaling,
degenerate curves, metric scaling, invalid inputs, unattainable tolerance and
native read-only/cache guards. Native tests are headless; artist validation in
an OS window remains on the worklist.

This batch is stacked on degree-elevation PR #164 until that PR is integrated.
