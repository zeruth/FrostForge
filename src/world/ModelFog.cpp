#include "world/ModelFog.hpp"

#include <cstring>

// ref: FUN_1402394a0
MODELFOG::MODELFOG() {
    std::memset(this->m_words, 0, sizeof(this->m_words));
    this->m_words[0] = 0xffffffff;
    this->m_words[0xc] = 0xffffffff;
    this->m_words[0xd] = 0xffffffff;
    this->m_words[0xe] = 0xffffffff;
    this->m_words[2] = 0x3f800000;
    this->m_words[3] = 0x3f800000;
    this->m_words[4] = 0x358637bd;
    this->m_words[5] = 0xc61c4000;
    this->m_words[7] = 0x3f800000;
    this->m_words[10] = 0x3a83126f;
    this->m_words[0xf] = 0x461c4000;
    this->m_words[0x12] = 0x3f800000;
    this->m_words[0x19] = 0x3f800000;
    this->m_words[0x1d] = 0x3f800000;
    this->m_words[0x21] = 0x3f800000;
    this->m_words[0x23] = 0x4b189680;
}

float MODELFOG::Get(int32_t word) const {
    float value;
    std::memcpy(&value, &this->m_words[word], 4);
    return value;
}

void MODELFOG::Set(int32_t word, float value) {
    std::memcpy(&this->m_words[word], &value, 4);
}

void MODELFOG::SetColor(uint32_t argb) {
    this->m_words[0] = argb;
    this->m_words[0xc] = argb;
    this->m_words[0xd] = argb;
    this->m_words[0xe] = argb;
}
