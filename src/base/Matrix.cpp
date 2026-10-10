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

// ref: FUN_141342190
void MatrixRotateQuaternion(float* m, const float* q) {
    float r[16] = {};
    QuaternionToRotation(q, r);
    r[3] = 0.0f;
    r[7] = 0.0f;
    r[11] = 0.0f;
    r[12] = 0.0f;
    r[13] = 0.0f;
    r[14] = 0.0f;
    r[15] = 1.0f;
    MatrixMultiply(m, r, m);
}

// ref: FUN_141340a20
float* MatrixFromQuaternionRotation(float* m, const float* q) {
    QuaternionToRotation(q, m);
    m[15] = 1.0f;
    m[3] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
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

// ref: FUN_141341ab0
float MatrixDeterminant(const float* m) {
    float a = m[5];
    float b = m[4];
    float c = m[9];
    float d = m[10];
    float e = m[7];
    float f = m[11];
    float g = m[15];
    float h = m[6];
    float i = m[8];
    float j = m[12];
    float k = m[14];
    float l = m[13];
    return ((((((h * f * l + a * d * g + e * c * k) - e * d * l) - h * c * g) - a * f * k) * m[0]
                - ((((d * b * g + h * f * j + e * i * k) - e * d * j) - h * i * g) - f * b * k) * m[1])
               + ((((b * c * g + a * f * j + e * i * l) - e * c * j) - i * a * g) - f * b * l) * m[2])
        - ((((b * c * k + a * d * j + h * i * l) - h * c * j) - i * a * k) - d * b * l) * m[3];
}

// The cofactors, transposed.
// ref: FUN_141340bf0
float* MatrixAdjoint(const float* m, float* out) {
    float r[16];
    for (int32_t row = 0; row < 4; row++) {
        for (int32_t column = 0; column < 4; column++) {
            // The 3x3 minor without `row` and `column`.
            float n[9];
            int32_t k = 0;
            for (int32_t y = 0; y < 4; y++) {
                if (y == row) {
                    continue;
                }
                for (int32_t x = 0; x < 4; x++) {
                    if (x != column) {
                        n[k++] = m[y * 4 + x];
                    }
                }
            }
            float minor = n[0] * (n[4] * n[8] - n[5] * n[7]) - n[1] * (n[3] * n[8] - n[5] * n[6])
                + n[2] * (n[3] * n[7] - n[4] * n[6]);
            r[column * 4 + row] = ((row + column) & 1) != 0 ? -minor : minor;
        }
    }
    for (int32_t i = 0; i < 16; i++) {
        out[i] = r[i];
    }
    return out;
}

// The adjoint over `determinant`.
// ref: FUN_141341ef0
float* MatrixInverse(const float* m, float* out, float determinant) {
    float adjoint[16];
    MatrixAdjoint(m, adjoint);
    float scale = 1.0f / determinant;
    for (int32_t i = 0; i < 16; i++) {
        out[i] = adjoint[i] * scale;
    }
    return out;
}

// In place, for a rotation scaled by `scale` and moved: the rotation transposed (over the
// scale squared when the scale is not 1) and the translation taken back through it.
// ref: FUN_141341850
void MatrixAffineInverse(float* m, float scale) {
    float m8 = m[8];
    if (9.536743e-07f <= std::fabs(scale - 1.0f)) {  // DAT_14261a958
        float m1 = m[1];
        float m2 = m[2];
        float m6 = m[6];
        float m9 = m[9];
        float m4 = m[4];
        float s = 1.0f / (scale * scale);
        float m0 = m[0];
        float tx = -m[12];
        m[1] = m4 * s;
        m[4] = s * m1;
        float m5 = m[5];
        float ty = -m[13];
        m[2] = m8 * s;
        float tz = -m[14];
        m[0] = s * m0;
        float m10 = m[10];
        m[5] = s * m5;
        m[6] = m9 * s;
        m[8] = s * m2;
        m[9] = s * m6;
        m[10] = s * m10;
        m[12] = s * m1 * ty + s * m0 * tx + tz * s * m2;
        m[13] = m4 * s * tx + s * m5 * ty + tz * s * m6;
        m[14] = m8 * s * tx + m9 * s * ty + tz * s * m10;
    } else {
        float m1 = m[1];
        float m4 = m[4];
        float m2 = m[2];
        float m6 = m[6];
        float m9 = m[9];
        m[4] = m1;
        m[2] = m8;
        m[1] = m4;
        m[8] = m2;
        m[6] = m9;
        m[9] = m6;
        float tx = -m[12];
        float ty = -m[13];
        float tz = -m[14];
        m[12] = tx * m[0] + ty * m1 + tz * m2;
        m[13] = ty * m[5] + m4 * tx + tz * m6;
        m[14] = m8 * tx + ty * m9 + tz * m[10];
    }
}

// A reverse-depth perspective from a diagonal field of view: near maps to 1, far to 0.
// ref: FUN_14068e4b0
void MatrixPerspective(float fieldOfView, float aspect, float zNear, float zFar, float* m) {
    float half = (1.0f / std::sqrt(aspect * aspect + 1.0f)) * fieldOfView * 0.5f;
    float t = std::tan(half);
    m[3] = 0.0f;
    m[4] = 0.0f;
    m[8] = 0.0f;
    m[9] = 0.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[1] = 0.0f;
    m[2] = 0.0f;
    m[6] = 0.0f;
    m[7] = 0.0f;
    m[11] = 1.0f;
    m[15] = 0.0f;
    m[5] = zNear / (t * zNear);
    m[0] = zNear / (t * zNear * aspect);
    m[14] = (-zNear * zFar) / (zNear - zFar);
    m[10] = zNear / (zNear - zFar);
}

// A view looking from `eye` along `direction` with `up`: the columns are the side, the up and
// the direction, and the eye taken back through them.
// ref: FUN_14068de30
void MatrixLookAt(const float* eye, const float* direction, const float* up, float* m) {
    MatrixIdentity(m);
    float dz = direction[2] - eye[2];
    float dx = direction[0] - eye[0];
    float dy = direction[1] - eye[1];
    // DAT_142636334
    if (dx * dx + dy * dy + dz * dz < 0.01f || up[0] * up[0] + up[1] * up[1] + up[2] * up[2] < 0.01f) {
        return;
    }
    float inv = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
    dz = dz * inv;
    dx = dx * inv;
    dy = dy * inv;
    float sx;
    float sy;
    float sz;
    // DAT_1426c6f7c * DAT_142632534
    if (1.1920929e-07f * 2.0f <= std::fabs(std::fabs(dz) - 1.0f)) {
        sz = up[1] * dx - up[0] * dy;
        sy = up[0] * dz - up[2] * dx;
        sx = up[2] * dy - up[1] * dz;
        float s = 1.0f / std::sqrt(sy * sy + sx * sx + sz * sz);
        sy = sy * s;
        sx = s * sx;
        sz = sz * s;
    } else {
        sy = 0.0f;
        sz = 0.0f;
        sx = 1.0f;
    }
    float uz = sx * dy - dx * sy;
    float uy = dx * sz - sx * dz;
    float ux = sy * dz - sz * dy;
    float u = 1.0f / std::sqrt(uy * uy + ux * ux + uz * uz);
    m[0] = sx;
    m[1] = ux * u;
    m[2] = dx;
    m[4] = sy;
    m[5] = uy * u;
    m[6] = dy;
    m[8] = sz;
    m[9] = uz * u;
    m[10] = dz;
    float move[3] = {-eye[0], -eye[1], -eye[2]};
    MatrixTranslate(m, move);
}

// ref: FUN_1413416d0
float* MatrixRigidInverse(const float* m, float* out) {
    // FUN_1413449a0: the 3x3 part
    float r[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
    float tx = -m[12];
    float ty = -m[13];
    float tz = -m[14];
    out[3] = 0.0f;
    out[4] = r[1];
    out[7] = 0.0f;
    out[8] = r[2];
    out[11] = 0.0f;
    out[15] = 1.0f;
    out[1] = r[3];
    out[2] = r[6];
    out[5] = r[4];
    out[6] = r[7];
    out[9] = r[5];
    out[14] = ty * r[7] + tx * r[6] + tz * r[8];
    out[10] = r[8];
    out[13] = ty * r[4] + tx * r[3] + tz * r[5];
    out[0] = r[0];
    out[12] = ty * r[1] + tx * r[0] + tz * r[2];
    return out;
}

// ref: FUN_141348d40
float* BoxTransform(float* out, const float* box, const float* m) {
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    out[3] = m[12];
    out[4] = m[13];
    out[5] = m[14];
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t row = 0; row < 3; row++) {
            float a = m[row * 4 + i] * box[row];
            float b = m[row * 4 + i] * box[row + 3];
            float low = a;
            float high = b;
            if (a < b) {
                low = a;
                high = b;
            } else {
                low = b;
                high = a;
            }
            out[i] += low;
            out[i + 3] += high;
        }
    }
    return out;
}

// ref: FUN_141349220
float* BoxFromSpheres(float* out, const float* spheres, uint32_t count) {
    if (count == 0) {
        for (int32_t i = 0; i < 6; i++) {
            out[i] = 0.0f;
        }
        return out;
    }
    float r = spheres[3];
    float minX = spheres[0] - r;
    float minY = spheres[1] - r;
    float minZ = spheres[2] - r;
    float maxX = spheres[0] + r;
    float maxY = spheres[1] + r;
    float maxZ = spheres[2] + r;
    for (uint32_t i = 1; i < count; i++) {
        const float* s = spheres + i * 4;
        float sr = s[3];
        if (maxX < s[0] + sr) {
            maxX = s[0] + sr;
        }
        if (s[0] - sr < minX) {
            minX = s[0] - sr;
        }
        if (maxY < s[1] + sr) {
            maxY = s[1] + sr;
        }
        if (s[2] - sr < minZ) {
            minZ = s[2] - sr;
        }
        if (s[1] - sr < minY) {
            minY = s[1] - sr;
        }
        if (maxZ < s[2] + sr) {
            maxZ = s[2] + sr;
        }
    }
    out[0] = minX;
    out[1] = minY;
    out[2] = minZ;
    out[3] = maxX;
    out[4] = maxY;
    out[5] = maxZ;
    return out;
}

// ref: FUN_141348eb0
float* SphereFromSpheres(float* out, const float* spheres, uint32_t count) {
    if (count == 0) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
        return out;
    }
    if (count == 1) {
        out[0] = spheres[0];
        out[1] = spheres[1];
        out[2] = spheres[2];
        out[3] = spheres[3];
        return out;
    }
    float box[6];
    BoxFromSpheres(box, spheres, count);
    if (box[3] <= box[0] || box[4] <= box[1] || box[5] <= box[2]) {
        out[0] = box[0];
        out[1] = box[1];
        out[2] = box[2];
        out[3] = 0.0f;
        return out;
    }
    float cx = (box[3] + box[0]) * 0.5f;  // DAT_14261a988
    float cy = (box[4] + box[1]) * 0.5f;
    float cz = (box[5] + box[2]) * 0.5f;
    float radius = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        const float* s = spheres + i * 4;
        float d2 = (s[1] - cy) * (s[1] - cy) + (s[0] - cx) * (s[0] - cx) + (s[2] - cz) * (s[2] - cz);
        float gap = radius - s[3];
        if (gap < 0.0f || gap * gap < d2) {
            radius = std::sqrt(d2) + s[3];
        }
    }
    out[0] = cx;
    out[1] = cy;
    out[2] = cz;
    out[3] = radius;
    return out;
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

// ref: FUN_141342710
float* MatrixFromAxisAngle(float* m, float radians, const float* axis, bool normalized) {
    float x = axis[0];
    float y = axis[1];
    float z = axis[2];
    if (!normalized) {
        float scale = 1.0f / std::sqrt(y * y + x * x + z * z);
        x *= scale;
        y *= scale;
        z *= scale;
    }
    // FUN_14240ad20
    float s = std::sin(radians);
    float c = std::cos(radians);
    float t = 1.0f - c;
    m[15] = 1.0f;
    m[3] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    float xy = y * x * t;
    float xz = z * x * t;
    m[0] = x * x * t + c;
    m[1] = xy + z * s;
    m[4] = xy - z * s;
    m[2] = xz - y * s;
    float yz = z * y * t;
    m[8] = xz + y * s;
    m[5] = y * y * t + c;
    m[10] = z * z * t + c;
    m[6] = yz + x * s;
    m[9] = yz - x * s;
    return m;
}

// The axis is to x from; about z when the two are parallel.
// ref: FUN_1412b2570
float* MatrixRotationBetween(float* m, const float* from, const float* to) {
    float axis[3] = {
        to[2] * from[1] - to[1] * from[2],
        to[0] * from[2] - from[0] * to[2],
        from[0] * to[1] - to[0] * from[1],
    };
    float length = std::sqrt(axis[1] * axis[1] + axis[0] * axis[0] + axis[2] * axis[2]);
    if (std::fabs(length) < 9.5367432e-07f) {
        axis[0] = 0.0f;
        axis[1] = 0.0f;
        axis[2] = 1.0f;
    }
    // FUN_142412a68
    float angle = std::atan2(length, from[0] * to[0] + to[1] * from[1] + from[2] * to[2]);
    return MatrixFromAxisAngle(m, angle, axis, false);
}

// ref: FUN_141342ee0
void MatrixAddTranslation(float* m, const float* move) {
    m[12] = move[0] + m[12];
    m[13] = move[1] + m[13];
    m[14] = move[2] + m[14];
}
