#ifndef DVE_RANDOM_SAMPLING_HLSLI
#define DVE_RANDOM_SAMPLING_HLSLI

// PCG hash (O'Neill), the standard cheap, decent-quality integer hash for per-pixel/per-sample
// seeding in real-time renderers; not cryptographic, just needs to decorrelate neighboring
// pixels and successive samples well enough that AO doesn't show banding artifacts.
uint PcgHash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 Hash2(uint seed) {
    uint a = PcgHash(seed);
    uint b = PcgHash(a);
    return float2(a, b) / 4294967295.0F;
}


// Van der Corput radical inverse in base two, used by the Hammersley sequence.
float RadicalInverseBase2(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10F;
}

float2 Hammersley2D(uint sampleIndex, uint sampleCount) {
    return float2((float(sampleIndex) + 0.5F) / float(max(sampleCount, 1u)),
                  RadicalInverseBase2(sampleIndex));
}

// A per-pixel Cranley-Patterson rotation keeps the low-discrepancy samples stratified inside
// one frame while decorrelating pixels and frames for temporal accumulation.
float2 RotatedHammersley2D(uint sampleIndex, uint sampleCount, uint seed) {
    return frac(Hammersley2D(sampleIndex, sampleCount) + Hash2(seed));
}

// An orthonormal basis around `normal` (Duff et al.'s branchless construction), used to rotate
// a hemisphere sample generated in "local Z-up" space into world space around the surface
// normal.
void BuildOrthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent) {
    float sign = normal.z >= 0.0F ? 1.0F : -1.0F;
    float a = -1.0F / (sign + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0F + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
}

// Cosine-weighted hemisphere sample around `normal`, from a uniform 2D random pair in [0,1).
// Cosine weighting means the PDF cancels the NdotL term exactly, so an AO/ambient estimator
// using these samples doesn't need to divide by a PDF at all: the occlusion fraction across N
// samples is already the correctly-weighted estimate.
float3 CosineWeightedHemisphereSample(float3 normal, float2 uniformRandom) {
    float radius = sqrt(uniformRandom.x);
    float theta = 6.283185307179586F * uniformRandom.y;
    float x = radius * cos(theta);
    float y = radius * sin(theta);
    float z = sqrt(max(0.0F, 1.0F - uniformRandom.x));

    float3 tangent, bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);
    return normalize(tangent * x + bitangent * y + normal * z);
}

#endif // DVE_RANDOM_SAMPLING_HLSLI
