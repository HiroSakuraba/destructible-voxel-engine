#include "dve/asset_cooker.hpp"

namespace dve {

Matrix4 Matrix4::identity() noexcept {
    Matrix4 result;
    result.values[0] = 1.0F;
    result.values[5] = 1.0F;
    result.values[10] = 1.0F;
    result.values[15] = 1.0F;
    return result;
}

Matrix4 multiply(const Matrix4& a, const Matrix4& b) noexcept {
    Matrix4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k) {
                value += a.values[static_cast<std::size_t>(k * 4 + row)] *
                         b.values[static_cast<std::size_t>(column * 4 + k)];
            }
            result.values[static_cast<std::size_t>(column * 4 + row)] = value;
        }
    }
    return result;
}

Float3 transform_point(const Matrix4& matrix, Float3 point) noexcept {
    return {
        matrix.values[0] * point.x + matrix.values[4] * point.y + matrix.values[8] * point.z + matrix.values[12],
        matrix.values[1] * point.x + matrix.values[5] * point.y + matrix.values[9] * point.z + matrix.values[13],
        matrix.values[2] * point.x + matrix.values[6] * point.y + matrix.values[10] * point.z + matrix.values[14],
    };
}

Float3 transform_vector(const Matrix4& matrix, Float3 vector) noexcept {
    return {
        matrix.values[0] * vector.x + matrix.values[4] * vector.y + matrix.values[8] * vector.z,
        matrix.values[1] * vector.x + matrix.values[5] * vector.y + matrix.values[9] * vector.z,
        matrix.values[2] * vector.x + matrix.values[6] * vector.y + matrix.values[10] * vector.z,
    };
}

} // namespace dve
