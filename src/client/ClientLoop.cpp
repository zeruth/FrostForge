#include "client/FrameBufferCache.hpp"
#include "client/GxWindow.hpp"
#include "gx/GxDevice.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "client/Startup.hpp"
#include "cas/CAS.hpp"
#include "console/Command.hpp"
#include "fileservices/AsyncFile.hpp"
#include "gx/Gx.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "os/Window.hpp"
#include "ui/CSimpleTop.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>

// The main loop: CommonMain runs Sub140225AB0 while Sub1402225D0 returns true. The condition
// pumps the OS events into the client and advances the game state; the iteration renders.

// The loop's stubs (StartupClient.cpp).
bool Sub14134F980();
bool Sub140239E90();
uint32_t* Sub140239E10(uint32_t* out, const uint32_t* size);
bool Sub1405DC320();
bool Sub1405DBDE0();
bool Sub14026BEA0();
void Sub1413544D0();
void Sub14167BA90(uint64_t a1, uint64_t a2);
void UiRender(void* ui);
void Sub14022EA60();
void Sub1405F27E0(bool a1);
void Sub140238D50();
void Sub14022EE80(GxRenderContext* context, GXRENDERTARGETS* targets, bool a3);
void Sub14192FFC0(GxRenderContext* context, GXRENDERTARGETS* targets);
void Sub141350FC0();
bool Sub141C9CBD0();
void Sub140239EC0(FRAMEBUFFER* frame);
void Sub1412C51B0();
void Sub14068BC30();
void Sub14068AAE0();
void Sub14023D170(GXFRAMESTATS* stats);
void Sub14065FCE0(uint32_t elapsed, uint32_t now);
void Sub14065FD40();
void Sub14135A290();
void Sub140222A60();
void Sub14021F090();
void Sub1415642A0();
void Sub14230B040();
void Sub1412E1C00();
void Sub1412CAB50(uint64_t a1);
void Sub1412E1FB0();
void Sub1402F4D00(uint64_t a1);
void Sub14021C040();
void Sub14228C670();
void Sub1414F8E20();
void Sub141350FD0(bool a1);
void* Sub14192FA70();
void Sub14192FCB0(void* a1);
void Sub1413528E0(bool a1);
void Sub1412EE600();
void Sub1412EA4A0();
void Sub1412E8790();
void Sub14167C520();
void Sub141687ED0();
void Sub141691B00();
void Sub141525060(void* a1, float elapsed);
void Sub140234DF0();
void Sub140234ED0();
void Sub140235040();
void Sub140234080();
void Sub140570630();
void Sub14228CB00();
void ClientOnMouseCapture(uint32_t x, uint32_t y);
void UiInputChar(void* input, uint32_t character);
void UiInputIme(void* input, const void* event);
void ClientOnFocus(uint32_t active);
void ClientKeyBinding(uint32_t key, uint32_t down, char* handled);
bool ConsoleIsToggleKey(uint32_t key);
void OsWindowFocus(void* window);
void UiInputKeyDown(void* input, const void* event);
void UiInputKeyDownBound(void* input, const void* event);
void ClientOnKeyUp(const uint32_t* params, uint32_t time);
uint64_t Sub14152CF90(uint32_t a1);
void UiInputGamepadDown(void* input, const void* event);
void UiInputGamepadUp(void* input, const void* event);
void UiInputGamepadStick(void* input, const void* event);
void GxSetMouseCursorActive(uint32_t active);
void UiInputMouseDown(void* input, const void* event);
void UiInputMouseMove(void* input, const void* event, int32_t a3);
void UiInputMouseWheel(void* input, const void* event);
void UiInputMouseDelta(void* input, const void* event);
void ClientOnMouseUp(uint32_t button, uint32_t x, uint32_t y, uint32_t a4, uint32_t time, bool a6);
void UiInputDisplayChanged();
void Sub14065FB00();

namespace {

uint8_t s_quitRequested;            // DAT_142ddcf48
uint8_t s_quitHandled;              // DAT_142ddcfac
uint64_t s_quitArgument;            // DAT_142ddcf58
uint64_t s_quitArgument2;           // DAT_142ddcf60
float s_frameSeconds;               // DAT_142ddfa00
uint32_t s_frameTime;               // DAT_142ddf9f4
uint8_t s_paused;                   // DAT_142ddfa04
int32_t s_gameState;                // DAT_142ddfa08: 0 glue, 7 in world
uint8_t s_pendingReset;             // DAT_142ddf9f1
uint8_t s_pendingFlush;             // DAT_142ddf9f0
void* s_eventTarget;                // DAT_142de0e18
uint8_t s_eventsPaused;             // DAT_142de0e2c
uint8_t s_swallowChar;              // DAT_142de0e2d
uint32_t s_heldModifiers;           // DAT_142de0e24
uint32_t s_heldButtons;             // DAT_142de0e20
uint32_t s_pressedButtons;          // DAT_142de0e30
uint32_t s_mouseMode;               // DAT_142de0e34
int32_t* s_loadingScreen;           // DAT_142de0bf8

// What the loading screen is doing: true while it holds the game.
// ref: FUN_14022c010
bool LoadingScreenIsActive() {
    if (s_loadingScreen == nullptr) {
        return false;
    }
    if (*s_loadingScreen > 0) {
        return true;
    }
    FROST_UNIMPLEMENTED();  // the player's transport check (FUN_14170ff20 and on)
    return false;
}

// An OS event into the client: input goes to the UI's target, and close and quit stop the
// pump.
// ref: FUN_140236bc0
void ClientHandleEvent(void* context, uint32_t id, uint32_t* params, void* param) {
    (void)context;
    auto quit = static_cast<uint8_t*>(param);
    uint32_t time = params[3];
    switch (id) {
        case 0:
            ClientOnMouseCapture(params[1], params[2]);
            break;
        case 1:
            if (s_eventsPaused == 0) {
                if (s_swallowChar == 0) {
                    if (s_eventTarget != nullptr) {
                        UiInputChar(s_eventTarget, params[0] & 0x3fffffff);
                    }
                } else {
                    s_swallowChar = 0;
                }
            }
            break;
        case 2:
            if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                UiInputIme(s_eventTarget, params);
            }
            break;
        case 3:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x1b:
            break;
        case 4:
        case 0x14:
            *quit = 1;
            break;
        case 5:
            ClientOnFocus(params[0]);
            break;
        case 6: {
            uint32_t key = params[0] & 0x3fffffff;
            if (key < 6) {
                uint32_t bit = 1u << key;
                if ((s_heldModifiers & bit) != 0) {
                    break;
                }
                s_heldModifiers |= bit;
            }
            struct {
                uint32_t m_key;
                uint32_t m_time;
                uint8_t m_repeat;
                uint8_t m_system;
                uint16_t m_pad;
                uint32_t m_virtualKey;
                float m_scan;
            } event = { key, time, static_cast<uint8_t>((params[0] >> 30) & 1), static_cast<uint8_t>(params[0] >> 31), 0,
                params[1], static_cast<float>(params[2]) };
            char handled = 0;
            ClientKeyBinding(key, 1, &handled);
            if (handled == 0) {
                if (s_eventsPaused == 0) {
                    if (!ConsoleIsToggleKey(key)) {
                        if (s_eventTarget != nullptr) {
                            UiInputKeyDown(s_eventTarget, &event);
                        }
                    } else {
                        OsWindowFocus(OsGetMainWindow(0));
                        s_swallowChar = 1;
                    }
                }
            } else if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                UiInputKeyDownBound(s_eventTarget, &event);
            }
            break;
        }
        case 7:
            ClientOnKeyUp(params, time);
            break;
        case 8:
        case 9:
        case 10:
            if (s_eventsPaused == 0 && Sub14152CF90(0) != 0 && s_eventTarget != nullptr) {
                FROST_UNIMPLEMENTED();  // gamepad button and stick events (FUN_1405effd0 and its two siblings)
            }
            break;
        case 0xb:
        case 0x10:
        case 0xc:
        case 0x11:
        case 0xd:
        case 0x12:
            if (id == 0xb || id == 0x10) {
                s_heldButtons |= params[0];
                s_pressedButtons |= params[0];
            }
            if (s_eventsPaused == 0) {
                if ((id == 0xb || id == 0x10) && params[0] == 1) {
                    GxSetMouseCursorActive(1);
                }
                FROST_UNIMPLEMENTED();  // mouse events to the UI (normalized by the client rect, FUN_14123ab20)
            }
            break;
        case 0xe:
            if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                FROST_UNIMPLEMENTED();  // relative mouse motion to the UI (FUN_1405f1860)
            }
            break;
        case 0xf:
        case 0x13:
            ClientOnMouseUp(params[0], params[1], params[2], 0, time, id == 0x13);
            break;
        case 0x19:
            if (s_eventTarget != nullptr) {
                UiInputDisplayChanged();
            }
            break;
        case 0x1a:
            OsGetAsyncTimeMs();
            Sub14065FB00();
            break;
        case 0x1c:
            GxSetMouseCursorActive(0);
            break;
    }
}

// Runs the queued OS events into the client; false once something asked to quit.
// ref: FUN_1402379d0
bool ClientProcessEvents() {
    if (s_quitRequested != 0) {
        return false;
    }
    if (LoadingScreenIsActive()) {
        s_paused = 1;
    }
    s_eventTarget = CSimpleTop::s_instance;
    s_eventsPaused = s_paused;
    if (CSimpleTop::s_instance != nullptr) {
        CSimpleTop::s_instance->ApplyDeferredInputChanges();
    }
    // The deferred event handlers (DAT_142de0e00) run here when the events are not paused;
    // nothing adds to them yet.
    uint8_t quit = 0;
    OsEventPump(ClientHandleEvent, nullptr, &quit);
    s_eventTarget = nullptr;
    return quit == 0;
}

}

// Asks the client to quit: sets the flag ClientProcessEvents stops on and, the first time,
// lets the window and the connection go and drains the pump once more.
// ref: FUN_140216680
void ClientRequestQuit() {
    s_quitRequested = 1;
    if (s_quitHandled == 0) {
        FROST_UNIMPLEMENTED();  // FUN_1414f8d90 (window), FUN_1412d4870(0xfe, 1) and FUN_1412cab50 (the connection)
        Sub1412CAB50(s_quitArgument);
        uint8_t ignored;
        OsEventPump([](void*, uint32_t, uint32_t*, void*) {}, nullptr, &ignored);
        s_quitHandled = 1;
    }
}

// ref: FUN_140216570
uint64_t Sub140216570() {
    return s_quitArgument;
}

// ref: FUN_140216580
uint64_t Sub140216580() {
    return s_quitArgument2;
}

// The main loop's condition: pumps events and advances the game; false to stop.
// ref: FUN_1402225d0
bool Sub1402225D0() {
    GxFrameSetPhase(0);
    GxFrameLatencyWait();
    GxFrameWorkFlush(1);
    if (GxFrameLimited(s_gameState == 0 ? 0x3c : 0)) {
        GxFrameWorkFlush(2);
    }
    uint32_t now = OsGetAsyncTimeMs();
    if (now < g_startupValue && g_startupValue - now < 0x7fffffff) {
        now = g_startupValue;
    }
    uint32_t elapsed = now - g_startupValue;
    s_frameSeconds = static_cast<float>(elapsed) * 0.001f;  // DAT_142618f58
    g_startupValue = now;
    Sub14065FCE0(elapsed, now);
    s_frameTime = now;
    if (!ClientProcessEvents()) {
        return false;
    }
    Sub14065FD40();
    AsyncReadPoll();
    Sub14135A290();
    Sub140222A60();
    cas::CASPollUpdate();
    Sub14021F090();
    Sub1415642A0();
    Sub14230B040();
    if (s_pendingReset != 0) {
        Sub1412E1C00();
        s_pendingReset = 0;
    }
    Sub1412CAB50(Sub140216570());
    Sub1412E1FB0();
    Sub1402F4D00(Sub140216570());
    Sub14021C040();
    Sub14228C670();
    Sub1414F8E20();
    // The scheduled callbacks (DAT_142cc8060, with additions at DAT_142cc8300 and removals at
    // DAT_142cc8460) run here; nothing schedules any yet.
    Sub141350FD0(s_gameState == 7);
    Sub14192FCB0(Sub14192FA70());
    Sub1413528E0(s_gameState != 7 || LoadingScreenIsActive());
    switch (s_gameState) {
        case 0:
            s_paused = 0;
            Sub1412EE600();
            Sub1412EA4A0();
            Sub1412E8790();
            Sub14167C520();
            Sub141687ED0();
            Sub141691B00();
            CSimpleTop::s_instance->OnUpdate(s_frameSeconds);
            Sub141525060(nullptr, s_frameSeconds);  // &DAT_142d082d0
            break;
        case 1:
            s_gameState = 2;
            break;
        case 2:
            Sub140234DF0();
            break;
        case 3:
            Sub140234ED0();
            break;
        case 4:
            Sub140235040();
            break;
        case 5:
            Sub140234080();
            break;
        case 7:
            FROST_UNIMPLEMENTED();  // the world's update (FUN_1418e6b00 and on)
            Sub14228CB00();
            break;
    }
    if (s_pendingFlush != 0) {
        Sub140570630();
        s_pendingFlush = 0;
    }
    return true;
}

namespace {

uint8_t s_inFrame;                  // DAT_142ddfa10
uint32_t s_skippedFrames;           // DAT_142ddfa0c

// Colour target `index` of the frame buffer takes the texture's format and usage, and the
// texture itself.
// ref: FUN_140226150
void FrameBufferUseTexture(FRAMEBUFFER* buffer, uint32_t index, void* texture) {
    auto tex = static_cast<const gxDx12Backend::Tex*>(texture);
    uint32_t usage = tex->m_usage;
    FRAMEBUFFERTARGET* target = &buffer->m_color[index];
    std::memset(target, 0, sizeof(*target));
    target->m_format = tex->m_format;
    target->m_samples = (usage >> 0x15) & 7;
    target->m_08 = (usage >> 0x18) & 3;
    target->m_renderTarget = 1;
    target->m_unordered = static_cast<uint8_t>((usage >> 0x14) & 1);
    target->m_shaderResource = static_cast<uint8_t>((usage >> 0x13) & 1);
    GxRenderTargetsAttach(&buffer->m_targets, static_cast<uint8_t>(index + 1), static_cast<gxDx12Backend::Tex*>(texture));
}

// The frame end's callback: the end-of-frame upkeep, the frame stats, and the frame-rate cap
// (60 at the glue screens).
// ref: FUN_1402261d0
void OnFrameEnded(GXFRAMESTATS* stats, void* storage) {
    (void)storage;
    Sub14068BC30();
    Sub14068AAE0();
    Sub14023D170(stats);
    GxFrameLimit(s_gameState == 0 ? 0x3c : 0);
}

}

// One frame: the world or the glue screens drawn into the window's frame buffer, which is
// then copied to the swap chain's back buffer and presented.
// ref: FUN_140225ab0
void Sub140225AB0() {
    GxFrameSetPhase(1);
    s_inFrame = 1;
    if (!LoadingScreenIsActive() && s_gameState == 6) {
        s_skippedFrames++;
        s_inFrame = 0;
        return;
    }
    if (!Sub14134F980() && (LoadingScreenIsActive() || s_gameState != 7)) {
        Sub1413528E0(true);
    }
    GxFrameWorkFlush(2);
    bool overlapped = GxFrameArm(true) && (s_gameState == 0 || s_gameState == 7);
    if (!GxFrameBegin()) {
        Sub1412C51B0();
        s_inFrame = 0;
        return;
    }
    auto chain = static_cast<GXSWAPCHAIN*>(WindowGetSwapChain());
    if (GxSwapChainBegin(chain)) {
        FRAMEBUFFER* frame = WindowGetFrameBuffer();
        GXRENDERTARGETS* targets = &frame->m_targets;
        GxRenderTargetsClear(targets);
        uint32_t size = *reinterpret_cast<const uint32_t*>(&chain->m_width);
        if (Sub140239E90()) {
            uint32_t scaled;
            size = *Sub140239E10(&scaled, &size);
        }
        FrameBufferSetup(frame, chain->m_format, size);
        bool present = true;
        if (!LoadingScreenIsActive()) {
            if (s_gameState == 7) {
                FROST_UNIMPLEMENTED();  // FUN_141472dc0 (the world scene), the UI and FUN_1405f27e0
                if (s_paused != 0) {
                    s_paused = 0;
                    present = false;
                }
            } else if (s_gameState == 0) {
                FrameBufferBind(frame);
                g_gxRenderContext->Clear(targets->m_count != 0 ? targets : nullptr, 0xf, 0xff000000, 1.0f, 0, 0);
                Sub1413544D0();
                Sub14167BA90(Sub140216570(), Sub140216580());
                UiRender(CSimpleTop::s_instance);
                Sub14022EA60();
                Sub1405F27E0(!Sub14026BEA0());
                if (!Sub1405DC320() && !Sub1405DBDE0() && !Sub140239E90()) {
                    void* back = GxSwapChainGetBackBuffer(chain);
                    int32_t sourceOrigin[2] = { 0, 0 };
                    int32_t destOrigin[2] = { 0, 0 };
                    g_gxRenderContext->CopyTex(static_cast<gxDx12Backend::Tex*>(targets->m_texs[1]),
                        static_cast<gxDx12Backend::Tex*>(back), sourceOrigin, destOrigin, reinterpret_cast<uint32_t*>(frame->m_size), 0, 0);
                    FrameBufferSetColor(frame, 0, back);
                    g_gxRenderContext->SetRenderTargets(targets->m_count != 0 ? targets : nullptr);
                }
            }
            Sub140238D50();
        } else {
            if (!Sub1405DC320() && !Sub1405DBDE0() && !Sub140239E90()) {
                FrameBufferUseTexture(frame, 0, GxSwapChainGetBackBuffer(chain));
            }
            FrameBufferBind(frame);
            Sub14022EE80(g_gxRenderContext, targets->m_count != 0 ? targets : nullptr, true);
        }
        if (frame->m_bound == 0) {
            FrameBufferUseTexture(frame, 0, GxSwapChainGetBackBuffer(chain));
            FrameBufferBind(frame);
            g_gxRenderContext->Clear(targets->m_count != 0 ? targets : nullptr, 1, 0, 1.0f, 0, 0);
        } else {
            if (Sub1405DC320() || Sub1405DBDE0()) {
                FROST_UNIMPLEMENTED();  // FUN_1412a0e30 with FUN_140225960 / FUN_1402259d0 (capture flips)
            }
            Sub14192FFC0(g_gxRenderContext, !LoadingScreenIsActive() && targets->m_count != 0 ? targets : nullptr);
            WindowDrawCursor();
        }
        Sub141350FC0();
        Sub141C9CBD0();
        Sub140239EC0(frame);
        if (present) {
            GxSwapChainPresent(chain);
        } else {
            GxSwapChainSkip(chain);
        }
        GxSwapChainEnd(chain);
        FrameBufferRelease(frame);
    }
    blz::function_storage callback = { reinterpret_cast<void*>(&OnFrameEnded), blz::function_empty_manager, {} };
    GxFrameEnd(&callback, overlapped);
    callback.m_manager(callback.m_storage, nullptr, 0);
    Sub1412C51B0();
    s_inFrame = 0;
}
