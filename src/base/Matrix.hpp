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
// Rotates by a quaternion, pre-multiplying (FUN_141342190).
void MatrixRotateQuaternion(float* m, const float* q);
// The same keeping nothing of the matrix's translation (FUN_141340a20).
float* MatrixFromQuaternionRotation(float* m, const float* q);
// A 0..1 colour as 0xffRRGGBB (FUN_141343fd0).
uint32_t ColorFromVector(const float* rgb);
// A colour moved toward `target` by alpha/256 per channel (alpha 255 copies it), its own alpha
// kept; alpha 0 leaves it.
// ref: FUN_1413da3d0
void ColorLerpByte(uint32_t* color, uint32_t alpha, uint32_t target);
// The real roots of a·t² + b·t + c, smaller first (the numerically stable form); false when
// there are none.
// ref: FUN_141343dd0
bool QuadraticRoots(float a, float b, float c, float* low, float* high);
// RGB (0..1) to HSV (hue in degrees, saturation, value); out of range gives (-1, 0, 0).
void ColorRgbToHsv(const float* rgb, float* hsv);
// HSV back to RGB; a hue of -1 with no saturation is grey.
void ColorHsvToRgb(const float* hsv, float* rgb);
// The index of the largest and the smallest of three magnitudes, and the largest of four.
uint32_t MaxAbsIndex3(const float* v);
uint32_t MinAbsIndex3(const float* v);
uint32_t MaxAbsIndex4(const float* v);
// A critically damped spring of four lanes toward target: omega the stiffness, dt the step.
// True when it settled (snapped to the target, velocity zeroed).
bool SmoothDamp4(float* position, float* velocity, const float* target, float omega, float dt, float epsilon);
// The determinant (FUN_141341ab0), the adjoint (FUN_141340bf0), the inverse as the adjoint
// over a determinant already found (FUN_141341ef0), and the in-place inverse of a uniformly
// scaled rotation and translation (FUN_141341850).
float MatrixDeterminant(const float* m);
float* MatrixAdjoint(const float* m, float* out);
float* MatrixInverse(const float* m, float* out, float determinant);
void MatrixAffineInverse(float* m, float scale);
// A reverse-depth perspective from a diagonal field of view (FUN_14068e4b0) and a look-at view
// (FUN_14068de30).
void MatrixPerspective(float fieldOfView, float aspect, float zNear, float zFar, float* m);
void MatrixPerspectiveVertical(float fieldOfView, float aspect, float zNear, float zFar, float* m);
void MatrixLookAt(const float* eye, const float* direction, const float* up, float* m);
// A rotation about a unit axis by an angle (FUN_141342710; `normalized` skips normalising the
// axis), the rotation turning `from` to `to` (FUN_1412b2570), and a translation added
// (FUN_141342ee0).
float* MatrixFromAxisAngle(float* m, float radians, const float* axis, bool normalized);
float* MatrixRotationBetween(float* m, const float* from, const float* to);
void MatrixAddTranslation(float* m, const float* move);
// A ray {origin, direction} against a plane {normal, d} (FUN_14134a420): where along the ray and
// the point it meets the plane; a ray along the plane meets it only when it starts within
// `epsilon` of it, at its origin.
bool RayPlaneIntersect(const float* ray, const float* plane, float* t, float* hit, float epsilon);
// Normalises unless the length is about zero (FUN_1411c9970).
void VectorNormalize(float* v);
// The inverse of a rotation and translation: the rotation transposed, the translation turned
// back through it (FUN_1413416d0).
float* MatrixRigidInverse(const float* m, float* out);
// Boxes are {min, max}; spheres {center, radius}. A box through a matrix, as the box around the
// transformed corners (FUN_141348d40); the box around spheres (FUN_141349220); the sphere around
// spheres, centred on that box (FUN_141348eb0).
float* BoxTransform(float* out, const float* box, const float* m);
float* BoxFromSpheres(float* out, const float* spheres, uint32_t count);
float* SphereFromSpheres(float* out, const float* spheres, uint32_t count);
// The inverse, or the identity when the determinant is about zero (FUN_141342930).
void MatrixInverseOrIdentity(const float* m, float* out);

// 3x3 matrices (NTempest's C33Matrix), row vectors, 9 floats: a times b (FUN_141344b20), from a
// quaternion (FUN_1413449e0), a rotation about an axis (FUN_141345dd0, `normalized` skips
// normalising it), a 4x4's 3x3 part (FUN_1413449a0) and every entry scaled (FUN_141345fc0).
float* Matrix3Multiply(float* out, const float* a, const float* b);
float* Matrix3FromQuaternion(float* out, const float* q);
float* Matrix3FromAxisAngle(float* out, float radians, const float* axis, bool normalized);
float* Matrix3FromMatrix(float* out, const float* m);
void Matrix3Scale(float* m, float scale);
// A quaternion from a 3x3 given column-major (FUN_141348170), and from a row-major one, which is
// transposed first (FUN_141347f30).
void QuaternionFromRotation(float* q, const float* m);
void QuaternionFromMatrix3(float* q, const float* m);
// The engine's quick sine and cosine: a parabola through each half turn (FUN_141343a90,
// FUN_1413437b0).
float FastSin(float radians);
float FastCos(float radians);

#endif
