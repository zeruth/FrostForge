#include "base/Coordinate.hpp"

#include <cmath>

COORDINATESPACE g_coordinateSpace = { 4.0f / 3.0f, 0.8f, 0.6f, 1.0f };

// ref: FUN_1402c9820
void CoordinateSetAspectRatio(COORDINATESPACE* space, float aspect) {
    space->m_aspectCompensation = aspect * 0.75f;
    space->m_aspect = aspect;
    float height = 1.0f / std::sqrt(aspect * aspect + 1.0f);
    space->m_diagonalHeight = height;
    space->m_diagonalWidth = height * aspect;
}
