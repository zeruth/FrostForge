#include "world/WorldFade.hpp"

#include "storm/Memory.hpp"

namespace {

const char s_fadeInfoTag[] = ".?AUInnerFadeInfo@@";

}  // namespace

// ref: FUN_141370120
CWorldFade::CWorldFade() {
    this->m_data = this->m_inline;
    this->m_capacity = 0x8000000000000008ull;
    this->m_size = 0;
    this->m_158 = 0;
    this->m_160 = 0;
    this->m_168 = 0;
    this->m_active = 0;
    this->m_174 = 0;
}

// ref: FUN_141370160
CWorldFade::~CWorldFade() {
    if (static_cast<int64_t>(this->m_capacity) >= 0) {
        SMemFree(this->m_data, s_fadeInfoTag, -6, 0);
        this->m_data = nullptr;
    }
}
