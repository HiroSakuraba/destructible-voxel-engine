#include "dve/render/ies_profile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <vector>

namespace dve::render {
namespace {

// IES data after the TILT= line is a free-form whitespace-separated token
// stream (not one value per line), so parsing tokenizes rather than splits
// lines — same observation as the NVIDIA reference implementation.
struct Tokenizer {
    std::string_view text;
    std::size_t pos = 0;
    bool next(std::string_view& token) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (pos >= text.size()) return false;
        const std::size_t start = pos;
        while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        token = text.substr(start, pos - start);
        return true;
    }
    bool next_float(float& value) {
        std::string_view token;
        if (!next(token)) return false;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        return result.ec == std::errc() && result.ptr == token.data() + token.size();
    }
    bool next_int(int& value) {
        std::string_view token;
        if (!next(token)) return false;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        return result.ec == std::errc() && result.ptr == token.data() + token.size();
    }
};

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

} // namespace

float IesProfile::sample(float angleRadians) const noexcept {
    const float degrees = std::clamp(angleRadians * 57.29577951308232F, 0.0F, 180.0F);
    const std::size_t lo = (std::min)(static_cast<std::size_t>(degrees),
                                      static_cast<std::size_t>(kIesProfileSampleCount - 2U));
    const float t = degrees - static_cast<float>(lo);
    return samples[lo] + t * (samples[lo + 1U] - samples[lo]);
}

bool parse_ies_profile(std::string_view text, IesProfile& out, std::string* error) {
    const std::size_t tiltPos = text.find("TILT=");
    if (tiltPos == std::string_view::npos) return fail(error, "IES file has no TILT= line");
    const std::size_t lineEnd = text.find_first_of("\r\n", tiltPos);
    if (lineEnd == std::string_view::npos) return fail(error, "IES TILT= line is unterminated");
    std::string_view tiltValue = text.substr(tiltPos + 5U, lineEnd - (tiltPos + 5U));
    while (!tiltValue.empty() && std::isspace(static_cast<unsigned char>(tiltValue.back())))
        tiltValue.remove_suffix(1);

    Tokenizer tok{text, lineEnd};
    if (tiltValue == "INCLUDE") {
        // Inline tilt block: geometry flag, pair count, then angle/multiplier
        // pairs. Consumed to stay positioned; the tilt correction is not
        // applied (documented in the header).
        int geometry = 0, pairs = 0;
        if (!tok.next_int(geometry) || !tok.next_int(pairs) || pairs < 0 || pairs > 1024)
            return fail(error, "IES TILT=INCLUDE block is malformed");
        float dummy = 0.0F;
        for (int i = 0; i < pairs * 2; ++i)
            if (!tok.next_float(dummy)) return fail(error, "IES TILT=INCLUDE block is truncated");
    }
    // TILT=NONE needs nothing; TILT=<file> keeps its data in the named file,
    // which is not loaded — the main file's photometrics parse normally.

    int numLamps = 0, numVertical = 0, numHorizontal = 0, photometricType = 0, unitsType = 0;
    float lumensPerLamp = 0.0F, candelaMultiplier = 0.0F, width = 0.0F, length = 0.0F, height = 0.0F;
    if (!tok.next_int(numLamps) || !tok.next_float(lumensPerLamp) ||
        !tok.next_float(candelaMultiplier) || !tok.next_int(numVertical) ||
        !tok.next_int(numHorizontal) || !tok.next_int(photometricType) ||
        !tok.next_int(unitsType) || !tok.next_float(width) || !tok.next_float(length) ||
        !tok.next_float(height))
        return fail(error, "IES lamp/photometric header line is malformed");
    (void)numLamps; (void)lumensPerLamp; (void)photometricType; (void)unitsType;
    (void)width; (void)length; (void)height;
    // 1024 per axis is far beyond any real file (181x361 at 1-degree steps)
    // and bounds the candela allocation.
    if (numVertical <= 0 || numHorizontal <= 0 || numVertical > 1024 || numHorizontal > 1024)
        return fail(error, "IES angle counts are out of range");

    float ballastFactor = 1.0F, unused = 0.0F, inputWatts = 0.0F;
    if (!tok.next_float(ballastFactor) || !tok.next_float(unused) || !tok.next_float(inputWatts))
        return fail(error, "IES ballast line is malformed");
    (void)unused; (void)inputWatts;

    std::vector<float> verticalAngles(static_cast<std::size_t>(numVertical));
    for (float& angle : verticalAngles)
        if (!tok.next_float(angle)) return fail(error, "IES vertical angles are truncated");
    std::vector<float> horizontalAngles(static_cast<std::size_t>(numHorizontal));
    for (float& angle : horizontalAngles)
        if (!tok.next_float(angle)) return fail(error, "IES horizontal angles are truncated");
    (void)horizontalAngles;

    std::vector<float> candela(static_cast<std::size_t>(numHorizontal) *
                               static_cast<std::size_t>(numVertical));
    for (float& value : candela) {
        if (!tok.next_float(value)) return fail(error, "IES candela values are truncated");
        value *= candelaMultiplier * ballastFactor;
    }

    // Resample each horizontal row onto the common 1-degree vertical grid,
    // then average the rows (azimuthal averaging; see the header).
    IesProfile parsed{};
    for (int h = 0; h < numHorizontal; ++h) {
        const float* row = &candela[static_cast<std::size_t>(h) *
                                    static_cast<std::size_t>(numVertical)];
        for (std::size_t i = 0; i < kIesProfileSampleCount; ++i) {
            const float theta = static_cast<float>(i);
            float value = 0.0F;
            if (theta <= verticalAngles.front()) {
                value = row[0];
            } else if (theta >= verticalAngles.back()) {
                value = row[static_cast<std::size_t>(numVertical) - 1U];
            } else {
                const auto it = std::upper_bound(verticalAngles.begin(), verticalAngles.end(), theta);
                const std::size_t hi = static_cast<std::size_t>(it - verticalAngles.begin());
                const std::size_t lo = hi - 1U;
                const float span = verticalAngles[hi] - verticalAngles[lo];
                const float t = span > 0.0F ? (theta - verticalAngles[lo]) / span : 0.0F;
                value = row[lo] + t * (row[hi] - row[lo]);
            }
            parsed.samples[i] += value;
        }
    }
    for (float& s : parsed.samples) s /= static_cast<float>(numHorizontal);
    const float peak = *std::max_element(parsed.samples.begin(), parsed.samples.end());
    if (!(peak > 0.0F) || !std::isfinite(peak))
        return fail(error, "IES candela distribution has no positive peak");
    for (float& s : parsed.samples) s /= peak;
    out = parsed;
    return true;
}

} // namespace dve::render
