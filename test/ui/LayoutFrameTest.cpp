// Frames anchored to a fixed root solve their rects through the pending-resize pass: a point
// plus a size, two points, SetAllPoints, and the refusals (self, dependency loop).
// Exits non-zero on any failure.

#include <cmath>
#include <cstdio>

#include "ui/CLayoutFrame.hpp"
#include "ui/UITypes.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

void CheckRect(CLayoutFrame* frame, float left, float bottom, float right, float top, const char* what) {
    CRect rect;
    bool ok = frame->GetRect(&rect) && Near(rect.minX, left) && Near(rect.minY, bottom) && Near(rect.maxX, right)
        && Near(rect.maxY, top);
    if (!ok) {
        std::printf("  %s: got %g %g %g %g\n", what, rect.minX, rect.minY, rect.maxX, rect.maxY);
    }
    Check(ok, what);
}

}

int main() {
    CLayoutFrame::CreateResizeAllocator();

    CLayoutFrame root;
    root.m_rect = {0.0f, 0.0f, 0.6f, 0.8f};
    root.m_flags |= CLayoutFrame::FLAG_RECT_VALID;

    CLayoutFrame child;
    child.m_layoutState |= CLayoutFrame::LAYOUT_VISIBLE;
    child.SetSize(0.2f, 0.1f);
    Check(child.SetPoint(FRAMEPOINT_TOPLEFT, &root, FRAMEPOINT_TOPLEFT, 0.1f, -0.1f, true) == 0, "SetPoint");
    Check(child.IsDependentOn(&root), "anchored to the root");
    CLayoutFrame::ResizePending();
    CheckRect(&child, 0.1f, 0.4f, 0.3f, 0.5f, "one point and a size");

    CLayoutFrame grandchild;
    grandchild.m_layoutState |= CLayoutFrame::LAYOUT_VISIBLE;
    grandchild.SetPoint(FRAMEPOINT_TOPLEFT, &child, FRAMEPOINT_BOTTOMLEFT, 0.0f, 0.0f, true);
    grandchild.SetPoint(FRAMEPOINT_BOTTOMRIGHT, &root, FRAMEPOINT_BOTTOMRIGHT, 0.0f, 0.0f, true);
    CLayoutFrame::ResizePending();
    CheckRect(&grandchild, 0.1f, 0.0f, 0.8f, 0.4f, "two points");

    Check(child.SetPoint(FRAMEPOINT_TOP, &grandchild, FRAMEPOINT_TOP, 0.0f, 0.0f, true) == 8, "a dependency loop is refused");
    Check(child.SetPoint(FRAMEPOINT_TOP, &child, FRAMEPOINT_TOP, 0.0f, 0.0f, true) == 9, "anchoring to itself is refused");

    // moving the child moves what hangs off it
    child.SetPoint(FRAMEPOINT_TOPLEFT, &root, FRAMEPOINT_TOPLEFT, 0.2f, -0.1f, true);
    CLayoutFrame::ResizePending();
    CheckRect(&child, 0.2f, 0.4f, 0.4f, 0.5f, "moved");
    CheckRect(&grandchild, 0.2f, 0.0f, 0.8f, 0.4f, "dependent followed");

    CLayoutFrame filler;
    filler.m_layoutState |= CLayoutFrame::LAYOUT_VISIBLE;
    Check(filler.SetAllPoints(&child, true) == 0, "SetAllPoints");
    CLayoutFrame::ResizePending();
    CheckRect(&filler, 0.2f, 0.4f, 0.4f, 0.5f, "all points");
    Check(filler.GetNumPoints() == 2, "SetAllPoints sets two points");

    filler.ClearAllPoints();
    Check(!filler.IsAnchored(), "cleared");

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "passed", s_failures);
    return s_failures ? 1 : 0;
}
