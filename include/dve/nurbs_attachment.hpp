#pragma once
#include "dve/nurbs.hpp"
#include <optional>
#include <string>
#include <stop_token>
namespace dve {
enum class NurbsBoundary { UMin, UMax, VMin, VMax };
struct NurbsAttachmentOptions {
    NurbsBoundary referenceBoundary{NurbsBoundary::UMax};
    NurbsBoundary movingBoundary{NurbsBoundary::UMin};
    bool reverseBoundary{};
    bool c1{};
    // Keep a stable boundary weight gauge when maintaining a persistent seam.
    bool preserveWeightGauge{};
    // Last, so existing positional initializers keep their meaning.
    // Also match second cross-boundary derivatives (C2, so curvature across the seam matches:
    // G2). Implies c1. The moving patch is degree-elevated to at least 2 with four controls
    // across, so its far edge stays put.
    bool c2{};
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
// C2 additionally matches homogeneous second derivatives (implies G2).
// Returns independent patches; no sewing, live adjacency or blend strip.
[[nodiscard]] std::optional<NurbsAttachment> attach_nurbs_boundaries(
    const NurbsSource& reference,const NurbsSource& moving,
    NurbsAttachmentOptions options={},std::string* error=nullptr,std::stop_token stop={});
}
