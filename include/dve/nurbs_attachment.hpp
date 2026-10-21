#pragma once
#include "dve/nurbs.hpp"
namespace dve {
enum class NurbsBoundary { UMin, UMax, VMin, VMax };
struct NurbsAttachmentOptions {
    NurbsBoundary referenceBoundary{NurbsBoundary::UMax};
    NurbsBoundary movingBoundary{NurbsBoundary::UMin};
    bool reverseBoundary{};
    bool c1{};
};
struct NurbsAttachment {
    // Shape-preserving compatible reference; the input itself is never modified.
    NurbsSource reference;
    NurbsSource moving;
};
// Match a moving boundary to a fixed reference. Tangential axes are normalized,
// degree-elevated and refined to the exact union of knot multiplicities (no fit
// or near-knot snapping). Outputs restore their input axes/domains and UV ranges.
// C1 matches homogeneous outward-reference/inward-moving derivatives in unit
// cross-boundary domains. Reverse maps moving edge fraction s to reference 1-s.
// Returns independent patches; no sewing, live adjacency, G2/C2 or blend strip.
[[nodiscard]] std::optional<NurbsAttachment> attach_nurbs_boundaries(
    const NurbsSource& reference,const NurbsSource& moving,
    NurbsAttachmentOptions options={},std::string* error=nullptr);
}
