#include "framescript/FrameScriptObject.hpp"

#include <cstring>

namespace {

// 0x142cf5510
const char* const s_scriptNames[FRAMESCRIPT_NUM_SCRIPTS] = {
    "onclick", "preclick", "postclick", "ondoubleclick", "onload", "onfinished", "onplay",
    "onpause", "onstop", "onsizechanged", "onupdate", "onloop", "onshow", "onhide", "onenter",
    "onleave", "onmousedown", "onmouseup", "onmousewheel", "ondragstart", "ondragstop",
    "onreceivedrag", "onchar", "onhyperlinkclick", "onhyperlinkenter", "onhyperlinkleave",
    "onevent", "onkeydown", "onkeyup", "ongamepadbuttondown", "ongamepadbuttonup",
    "ongamepadstick", "onattributechanged", "onenable", "ondisable", "onenterpressed",
    "onescapepressed", "onscrollrangechanged", "onverticalscroll", "onhorizontalscroll",
    "onvaluechanged", "ontabpressed", "oneditfocuslost", "oneditfocusgained", "ontextchanged",
    "onmodelloaded", "onmodelcleared", "onmodelloading", "ondressmodel", "onmovieshowsubtitle",
    "onmoviehidesubtitle", "onmoviefinished", "ontooltipsetdefaultanchor", "ontooltipcleared",
    "ontooltipaddmoney", "ontooltipsetunit", "ontooltipsetitem", "ontooltipsetspell",
    "ontooltipsetquest", "ontooltipsetachievement", "ontooltipsetequipmentset",
    "ontooltipsetframestack", "onspacepressed", "ontextset", "oninputlanguagechanged",
    "oncharcomposition", "onminmaxchanged", "oncursorchanged", "onanimstarted",
    "onanimfinished", "oncolorselect", "onexternallink", "onbuttonupdate", "onarrowpressed",
    "onerror", "onpanfinished", "oncooldowndone", "onrequestnewsize", "onuimapchanged",
};

}

// ref: FUN_140642a70
uint32_t FrameScript_GetScriptIndex(const char* name) {
    if (name != nullptr) {
        char lower[0x80];
        char* dest = lower;
        uint64_t left = 0x7f;
        while (true) {
            char c = *name++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        _strlwr(lower);
        for (uint32_t i = 0; i < FRAMESCRIPT_NUM_SCRIPTS; i++) {
            if (strcmp(lower, s_scriptNames[i]) == 0) {
                return i;
            }
        }
    }
    return FRAMESCRIPT_NUM_SCRIPTS + 1;
}
