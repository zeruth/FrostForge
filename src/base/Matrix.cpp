#include "base/Matrix.hpp"

// An orthographic projection (FUN_14068e3d0).
// ref: FUN_14068e3d0
void Orthographic(float left, float right, float bottom, float top, float zNear, float zFar, float* m) {
    m[3] = 0.0f;
    m[4] = 0.0f;
    m[8] = 0.0f;
    m[9] = 0.0f;
    m[1] = 0.0f;
    m[2] = 0.0f;
    m[6] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[15] = 1.0f;
    m[0] = 2.0f / (right - left);
    m[5] = 2.0f / (top - bottom);
    m[13] = -(bottom + top) / (top - bottom);
    m[12] = -(left + right) / (right - left);
    m[10] = 1.0f / (zFar - zNear);
    m[14] = -zNear / (zFar - zNear);
}

// ref: FUN_141342cc0
void MatrixScale(float* m, const float* scale) {
    m[0] = scale[0] * m[0];
    m[1] = m[1] * scale[0];
    m[2] = m[2] * scale[0];
    m[4] = m[4] * scale[1];
    m[5] = scale[1] * m[5];
    m[6] = scale[1] * m[6];
    m[8] = scale[2] * m[8];
    m[9] = m[9] * scale[2];
    m[10] = m[10] * scale[2];
}

// ref: FUN_141342e50
void MatrixTranslate(float* m, const float* move) {
    m[12] = m[4] * move[1] + move[0] * m[0] + m[8] * move[2] + m[12];
    m[13] = m[5] * move[1] + m[1] * move[0] + m[9] * move[2] + m[13];
    m[14] = m[6] * move[1] + m[2] * move[0] + m[10] * move[2] + m[14];
}
