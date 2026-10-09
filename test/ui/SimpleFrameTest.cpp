// Frames under a UI root: parenting, strata and level inheritance, showing and hiding down
// the tree, alpha, raising toplevel frames, and destruction. Exits non-zero on any failure.

#include <cstdio>

#include "ui/CLayoutFrame.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

uint32_t Strata(CSimpleFrame* frame) {
    return (frame->m_flags2 >> 7) & 0xf;
}

bool Visible(CSimpleFrame* frame) {
    return (frame->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) != 0;
}

bool Shown(CSimpleFrame* frame) {
    return (frame->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0;
}

}

int main() {
    CLayoutFrame::CreateResizeAllocator();
    auto top = new CSimpleTop(nullptr);

    auto parent = new CSimpleFrame(nullptr);
    Check(CSimpleFrame::s_count == 1, "one frame");
    Check(Shown(parent) && Visible(parent), "a new root frame is shown and visible");
    Check(Strata(parent) == 3, "medium strata by default");
    Check(parent->m_level == 0, "level 0 at the root");
    Check(parent->m_alpha == 0xff && parent->GetEffectiveAlpha() == 0xff, "opaque");
    Check((parent->m_flags2 & CSimpleFrame::FRAME2_LAYERS) == 0xf, "four layers drawn, highlight off");

    auto child = new CSimpleFrame(parent);
    Check(child->m_parent == parent, "parented");
    Check(parent->m_children.Head() != nullptr && parent->m_children.Head()->m_frame == child, "in the child list");
    Check(child->m_level == 1, "one level above the parent");
    Check(Visible(child), "visible under a visible parent");

    parent->SetFrameStrata(5);
    Check(Strata(child) == 5, "strata follows the parent");
    parent->SetFrameLevel(10);
    Check(parent->m_level == 10 && child->m_level == 11, "level offsets kept");

    parent->Hide();
    Check(!Visible(parent) && !Visible(child), "hiding hides the tree");
    Check(Shown(child), "the child keeps its own shown setting");
    parent->Show();
    Check(Visible(parent) && Visible(child), "showing shows the tree again");
    child->Hide();
    parent->Hide();
    parent->Show();
    Check(!Visible(child), "a hidden child stays hidden");
    child->Show();
    Check(Visible(child), "and shows on its own");

    child->SetAlpha(128);
    parent->SetAlpha(128);
    Check(child->m_parentAlpha == 128, "the parent's alpha reaches the child");
    Check(child->GetEffectiveAlpha() == 64, "alphas multiply");
    child->SetIgnoreParentAlpha(true);
    Check(child->GetEffectiveAlpha() == 128, "ignoring the parent's alpha");

    int32_t before = top->m_raiseCounter;
    parent->SetToplevel(true);
    Check((parent->m_frameFlags & CSimpleFrame::FRAME_OWN_BATCH) != 0, "a toplevel frame batches itself");
    Check((child->m_frameFlags & CSimpleFrame::FRAME_BATCHED) != 0, "its children are batched");
    child->Raise();
    Check(top->m_raiseCounter == before + 1 && parent->m_raiseOrder == top->m_raiseCounter, "raising the toplevel ancestor");
    Check(child->m_raiseOrder == parent->m_raiseOrder, "the raise order reaches the child");

    auto other = new CSimpleFrame(nullptr);
    child->SetParent(other);
    Check(child->m_parent == other && parent->m_children.Head() == nullptr, "reparented");
    Check(child->m_level == 1 && Strata(child) == 3, "level and strata from the new parent");
    Check((child->m_frameFlags & CSimpleFrame::FRAME_BATCHED) == 0, "no longer batched");

    child->Destroy();
    Check(CSimpleFrame::s_count == 2, "destroyed");
    Check(other->m_children.Head() == nullptr, "gone from its parent");
    other->Destroy();
    parent->Destroy();
    Check(CSimpleFrame::s_count == 0, "all destroyed");

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "passed", s_failures);
    return s_failures ? 1 : 0;
}
