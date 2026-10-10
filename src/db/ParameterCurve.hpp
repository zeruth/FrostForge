#ifndef DB_PARAMETERCURVE_HPP
#define DB_PARAMETERCURVE_HPP

// ParameterCurve (WoW/Source/DB/DBEngine/ParameterCurve_C.cpp): every Curve record's
// CurvePoints, packed once into one point array and one encoded word per curve, and evaluated
// by curve id. A word with bit 0 set is a constant (the bits are the float); otherwise bits
// 1..3 are the interpolation (0 linear, 1 cosine, 2 Catmull-Rom, 3 quadratic, 4 cubic, 5 any
// order Bezier), bits 4..23 the first point and the top byte the point count.

#include <cstdint>

struct C2VECTORF {
    float x;
    float y;
};

// The rebuild after Curve or CurvePoint changed (and on the first call).
// ref: FUN_14230b080
void ParameterCurveBuild();
// Registers the DB2 watchers and builds the curves.
// ref: FUN_14230b600
void ParameterCurveInitialize();
// Rebuilds when a watched table changed.
// ref: FUN_14230b040
void ParameterCurveUpdate();

// The curve's value at `t` (0 for an unknown curve).
// ref: FUN_14230ce30
float ParameterCurveEvaluate(uint32_t curveId, float t);
// The curve's point count (0 for a constant).
// ref: FUN_14230d1e0
uint32_t ParameterCurveGetPointCount(uint32_t curveId);
// The curve's index-th point; false past the end.
// ref: FUN_14230d140
bool ParameterCurveGetPoint(uint32_t curveId, uint32_t index, C2VECTORF* point);
// ref: FUN_14230d240
bool ParameterCurveGetFirstPoint(uint32_t curveId, C2VECTORF* point);
// ref: FUN_14230d070
bool ParameterCurveGetLastPoint(uint32_t curveId, C2VECTORF* point);

#endif
