#ifndef BASE_MATRIX_HPP
#define BASE_MATRIX_HPP

// 4x4 matrix helpers (NTempest's C44Matrix), row vectors, 16 floats.

// An orthographic projection (FUN_14068e3d0).
void Orthographic(float left, float right, float bottom, float top, float zNear, float zFar, float* m);
// Scales the first three rows (FUN_141342cc0).
void MatrixScale(float* m, const float* scale);
// Moves the origin by a vector in the matrix's space (FUN_141342e50).
void MatrixTranslate(float* m, const float* move);

#endif
