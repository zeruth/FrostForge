// Exercises the global and per-instance object heaps: allocation, index round trip, reuse after
// free, growth past one block, and alignment. Exits non-zero on any failure.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "base/ObjectAlloc.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

}

int main() {
    uint32_t heap = ObjectAllocAddHeap(0x20, 4, "TEST", 0x1f, false);
    std::vector<void*> objects;
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t index;
        void* object = ObjectAlloc(heap, &index, true);
        Check(object != nullptr, "alloc");
        Check(index == i, "indices run in order across blocks");
        objects.push_back(object);
        indices.push_back(index);
    }
    Check(static_cast<char*>(objects[1]) - static_cast<char*>(objects[0]) == 0x20, "objects packed in a block");
    ObjectFree(heap, indices[5]);
    // allocation keeps filling the current block (8..11) before it looks back for room
    uint32_t again;
    ObjectAlloc(heap, &again, false);
    Check(again == 10, "the current block fills first");
    ObjectAlloc(heap, &again, false);
    Check(again == 11, "the current block fills first");
    void* reused = ObjectAlloc(heap, &again, false);
    Check(reused == objects[5] && again == 5, "a freed slot comes back");

    uint32_t aligned = ObjectAllocAddHeap(0x14, 8, "ALIGN", 0x1f, true);
    for (uint32_t i = 0; i < 9; i++) {
        uint32_t index;
        void* object = ObjectAlloc(aligned, &index, false);
        Check((reinterpret_cast<uintptr_t>(object) & 0xf) == 0, "align16 objects are aligned");
    }

    TObjectAllocBase local;
    local.Init("LOCAL", 0x30, 8, 16, 0x1f, false, nullptr);
    void* a = local.Alloc(true);
    void* b = local.Alloc(true);
    Check(a != nullptr && b != nullptr && a != b, "instance alloc");
    Check(local.m_objSize == 0x38, "instance object size has room for the index");
    local.Free(a);
    void* c = local.Alloc(false);
    Check(c == a, "instance free reuses the slot");
    local.Destroy();

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "passed", s_failures);
    return s_failures ? 1 : 0;
}
