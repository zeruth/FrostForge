#ifndef BASE_MATRIX_HPP
#define BASE_MATRIX_HPP

// 4x4 matrix helpers (NTempest's C44Matrix), row vectors, 16 floats.

#include <cstdint>

// An orthographic projection (FUN_14068e3d0).
void Orthographic(float left, float right, float bottom, float top, float zNear, float zFar, float* m);
// Scales the first three rows (FUN_141342cc0).
void MatrixScale(float* m, const float* scale);
// Moves the origin by a vector in the matrix's space (FUN_141342e50).
void MatrixTranslate(float* m, const float* move);
// Rotates about an axis, post-multiplying (FUN_141342570, FUN_141342600, FUN_141342690).
void MatrixRotateX(float* m, float radians);
void MatrixRotateY(float* m, float radians);
void MatrixRotateZ(float* m, float radians);
// Scales the 3x3 part (FUN_141342d50).
void MatrixScaleUniform(float* m, float scale);
// out = a * b; the reference inlines it.
void MatrixMultiply(float* out, const float* a, const float* b);
void MatrixIdentity(float* m);
// Quaternions are x, y, z, w (C4Quaternion).
float* QuaternionFromEuler(float* q, float yaw, float pitch, float roll);  // FUN_141347da0
void QuaternionToRotation(const float* q, float* m);                      // FUN_1413408b0
float* MatrixFromQuaternion(float* m, const float* q);                    // FUN_1413409e0
// A 0..1 colour as 0xffRRGGBB (FUN_141343fd0).
uint32_t ColorFromVector(const float* rgb);
// Normalises unless the length is about zero (FUN_1411c9970).
void VectorNormalize(float* v);

#endif
