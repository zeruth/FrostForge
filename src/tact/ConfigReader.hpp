#ifndef TACT_CONFIG_READER_HPP
#define TACT_CONFIG_READER_HPP

// pkg/tact/3.6.39/src/config/config_reader.cpp: the "key = value" text of build and CDN
// configurations, read line by line into a visitor.

#include <cstdint>

namespace tact {

struct BUILDCONFIG;

// A visitor (vtables 0x1428e7db0 build, 0x1428e7dc8 CDN, 0x1428e7de0 the third kind).
class ConfigVisitor {
    public:
    virtual ~ConfigVisitor() = default;
    // +0x08: a line's key [key, keyEnd) and value [value, valueEnd); false stops the read.
    virtual bool OnKeyValue(const char* key, const char* keyEnd, const char* value, const char* valueEnd) = 0;
    // +0x10: the text ended; whether what was read is complete.
    virtual bool Finish() = 0;
};

// The build configuration visitor (its context is the BUILDCONFIG).
class BuildConfigVisitor : public ConfigVisitor {
    public:
    BUILDCONFIG* m_config;

    explicit BuildConfigVisitor(BUILDCONFIG* config) : m_config(config) {}
    bool OnKeyValue(const char* key, const char* keyEnd, const char* value, const char* valueEnd) override;
    bool Finish() override;
};

bool ConfigRead(ConfigVisitor* visitor, const char* text, uint32_t size);

// Decimal parsers with optional whitespace and sign: the characters consumed (0 for none);
// *error on overflow or a nonzero negative value.
uint64_t ParseUInt32(const char* text, uint64_t length, uint32_t* value, bool* error);
uint64_t ParseUInt8(const char* text, uint64_t length, uint8_t* value, bool* error);

}

#endif
