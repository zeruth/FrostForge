#ifndef UI_C_SIMPLE_FONT_HPP
#define UI_C_SIMPLE_FONT_HPP

// CSimpleFont (Engine/Source/Frame/CSimpleFont.cpp): a named font object FrameXML defines
// with <Font> and that font strings inherit from. Not ported yet.

struct XMLOBJECT;
class CStatus;

class CSimpleFont {
    public:
    // The font of a name, created when asked.
    static CSimpleFont* GetFont(const char* name, bool create);

    void LoadXML(XMLOBJECT* node, CStatus* status);
};

#endif
