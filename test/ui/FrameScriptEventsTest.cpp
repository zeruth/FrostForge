// Event names hash to ids the client knows (any case), unknown names do not, and the listener
// lists add, update, defer and remove as the reference does. Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "framescript/FrameScriptEvents.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

uint64_t Id(const char* name) {
    return FrameScriptEvents::GetEventID(name, std::strlen(name));
}

}

int main() {
    using namespace FrameScriptEvents;
    const char* known[] = {"ADDON_LOADED", "PLAYER_LOGIN", "UPDATE_BINDINGS", "VARIABLES_LOADED", "FRAMES_LOADED",
        "LOGIN_STATE_CHANGED", "CVAR_UPDATE", "DISPLAY_SIZE_CHANGED"};
    for (const char* name : known) {
        if (!IsKnownEvent(Id(name))) {
            std::printf("unknown: %s\n", name);
            s_failures++;
        }
    }
    Check(Id("addon_loaded") == Id("ADDON_LOADED"), "event ids ignore case");
    Check(!IsKnownEvent(Id("NOT_A_REAL_EVENT_NAME")), "an invented name is unknown");

    auto a = reinterpret_cast<FrameScript_Object*>(0x1000);
    auto b = reinterpret_cast<FrameScript_Object*>(0x2000);
    uint64_t login = Id("PLAYER_LOGIN");
    Check(RegisterEvent(a, login) == 0, "register");
    Check(RegisterEvent(a, login) == 1, "register twice");
    Check(RegisterEvent(a, Id("NOT_A_REAL_EVENT_NAME")) == 2, "register unknown");
    Check(IsEventRegistered(a, login), "registered");
    Check(!IsEventRegistered(b, login), "not registered");
    {
        DispatchScope scope(GetEventList(login));
        Check(RegisterEvent(b, login) == 0, "deferred register");
        Check(IsEventRegistered(b, login), "a deferred add counts as registered");
        Check(UnregisterEvent(a, login) == 0, "deferred unregister");
        Check(IsEventRegistered(a, login), "still listening until the dispatch ends");
    }
    Check(IsEventRegistered(b, login), "deferred add applied");
    Check(!IsEventRegistered(a, login), "deferred remove applied");
    Check(UnregisterEvent(a, login) == 1, "unregister when absent");
    RegisterAllEvents(a);
    blz::vector<FrameScript_Object*, s_objectTag> listeners;
    GetListeners(&listeners, login);
    Check(listeners.size() == 2 && listeners[0] == a && listeners[1] == b, "every-event listeners come first");
    Check(UnregisterAllEvents(b) == 0, "unregister all");
    Check(!IsEventRegistered(b, login), "gone after unregister all");
    Clear();

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "passed", s_failures);
    return s_failures ? 1 : 0;
}
