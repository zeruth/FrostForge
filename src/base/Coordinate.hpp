#ifndef BASE_COORDINATE_HPP
#define BASE_COORDINATE_HPP

// The screen shape UI coordinates are converted through. Lengths are kept as fractions of
// the screen diagonal, so the same layout keeps its proportions at any aspect ratio.
struct COORDINATESPACE {
    float m_aspect;                 // 0x00 width / height
    float m_diagonalWidth;          // 0x04 width as a fraction of the diagonal
    float m_diagonalHeight;         // 0x08 height as a fraction of the diagonal
    float m_aspectCompensation;     // 0x0C the aspect relative to 4:3
};

// DAT_142ccb9f0, 4:3 until the window's aspect is known.
extern COORDINATESPACE g_coordinateSpace;

void CoordinateSetAspectRatio(COORDINATESPACE* space, float aspect);

#endif
