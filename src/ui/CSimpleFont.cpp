#include "ui/CSimpleFont.hpp"

#include "util/Unimplemented.hpp"

// ref: FUN_14062aad0
CSimpleFont* CSimpleFont::GetFont(const char* name, bool create) {
    (void)name;
    (void)create;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14062b230
void CSimpleFont::LoadXML(XMLOBJECT* node, CStatus* status) {
    (void)node;
    (void)status;
    FROST_UNIMPLEMENTED();
}
