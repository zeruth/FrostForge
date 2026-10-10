#include "base/Matrix.hpp"

#include <cmath>

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

namespace {

// Rotates rows a and b of the matrix by an angle: a' = c a + s b, b' = c b - s a.
void RotateRows(float* m, int32_t a, int32_t b, float radians) {
    float sine = std::sin(radians);
    float cosine = std::cos(radians);
    for (int32_t i = 0; i < 4; i++) {
        float x = m[a * 4 + i];
        float y = m[b * 4 + i];
        m[a * 4 + i] = sine * y + x * cosine;
        m[b * 4 + i] = -sine * x + y * cosine;
    }
}

}

// ref: FUN_141342570
void MatrixRotateX(float* m, float radians) {
    RotateRows(m, 1, 2, radians);
}

// About Y the rows trade the other way: x' = c x - s z, z' = c z + s x.
// ref: FUN_141342600
void MatrixRotateY(float* m, float radians) {
    RotateRows(m, 2, 0, radians);
}

// ref: FUN_141342690
void MatrixRotateZ(float* m, float radians) {
    RotateRows(m, 0, 1, radians);
}

// ref: FUN_141342d50
void MatrixScaleUniform(float* m, float scale) {
    m[0] = scale * m[0];
    m[2] = scale * m[2];
    m[4] = scale * m[4];
    m[5] = scale * m[5];
    m[6] = scale * m[6];
    m[8] = scale * m[8];
    m[1] = scale * m[1];
    m[9] = scale * m[9];
    m[10] = scale * m[10];
}

// out = a * b (row vectors).
void MatrixMultiply(float* out, const float* a, const float* b) {
    float r[16];
    for (int32_t row = 0; row < 4; row++) {
        for (int32_t column = 0; column < 4; column++) {
            r[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column]
                + a[row * 4 + 2] * b[8 + column] + a[row * 4 + 3] * b[12 + column];
        }
    }
    for (int32_t i = 0; i < 16; i++) {
        out[i] = r[i];
    }
}

void MatrixIdentity(float* m) {
    for (int32_t i = 0; i < 16; i++) {
        m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
}

// A rotation from yaw, pitch and roll (about Z, Y and X), as x, y, z, w.
// ref: FUN_141347da0
float* QuaternionFromEuler(float* q, float yaw, float pitch, float roll) {
    float sa = std::sin(yaw * 0.5f);
    float ca = std::cos(yaw * 0.5f);
    float sb = std::sin(pitch * 0.5f);
    float cb = std::cos(pitch * 0.5f);
    float sc = std::sin(roll * 0.5f);
    float cc = std::cos(roll * 0.5f);
    float scsb = sc * sb;
    float sccb = sc * cb;
    float cccb = cc * cb;
    float ccsb = cc * sb;
    q[3] = cccb * ca + scsb * sa;
    q[2] = cccb * sa - scsb * ca;
    q[0] = sccb * ca - ccsb * sa;
    q[1] = ccsb * ca + sccb * sa;
    return q;
}

// ref: FUN_1413408b0
void QuaternionToRotation(const float* q, float* m) {
    float x = q[0];
    float y = q[1];
    float z = q[2];
    float w = q[3];
    float z2 = z + z;
    float y2 = y + y;
    float wx2 = w * (x + x);
    float xx2 = x * (x + x);
    m[0] = 1.0f - (z * z2 + y * y2);
    m[1] = x * y2 + w * z2;
    m[4] = x * y2 - w * z2;
    m[2] = x * z2 - w * y2;
    m[8] = x * z2 + w * y2;
    m[9] = y * z2 - wx2;
    m[5] = 1.0f - (z * z2 + xx2);
    m[6] = y * z2 + wx2;
    m[10] = 1.0f - (y * y2 + xx2);
}

// ref: FUN_1413409e0
float* MatrixFromQuaternion(float* m, const float* q) {
    m[3] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
    QuaternionToRotation(q, m);
    return m;
}

// A colour vector (0..1) as 0xffRRGGBB, rounded.
// ref: FUN_141343fd0
uint32_t ColorFromVector(const float* rgb) {
    uint32_t r = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(rgb[0] * 255.0f)));
    uint32_t g = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(rgb[1] * 255.0f)));
    uint32_t b = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(rgb[2] * 255.0f)));
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

// ref: FUN_1411c9970
void VectorNormalize(float* v) {
    float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (2.3841858e-07f < length) {  // DAT_14261a954
        float scale = 1.0f / std::sqrt(length);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
}
