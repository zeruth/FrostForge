// The glue's script API (FUN_140afd040): the namespaces (C_*), the Enum tables, the globals and
// the class metatables, registered into the glue's Lua state in the reference's order. Each
// registration not yet ported is a stub named for what it registers.

#include "glue/GlueScriptAPI.hpp"

#include "framescript/ScriptAPI.hpp"
#include "glue/ScriptAPIRegistry.hpp"

#include "ui/CSimpleButton.hpp"
#include "ui/CSimpleSlider.hpp"
#include "ui/CSimpleScrollFrame.hpp"
#include "ui/CSimpleCheckButton.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTexture.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lua.h"
}

namespace {

// the ModelScene metatables.
// ref: FUN_141a96680
void ScriptClass_ModelScene(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Checkout metatables.
// ref: FUN_141a98fe0
void ScriptClass_Checkout(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the CVar script callbacks (FUN_140aeeb70).
// ref: FUN_141168cf0
void ScriptSetCVarCallbacks_141168cf0() {
    FROST_UNIMPLEMENTED();
}

// the Animation metatables.
// ref: FUN_140606530
void ScriptClass_Animation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Alpha metatables.
// ref: FUN_1406064e0
void ScriptClass_Alpha(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the FlipBook metatables.
// ref: FUN_140606620
void ScriptClass_FlipBook(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the AnimationGroup metatables.
// ref: FUN_140606580
void ScriptClass_AnimationGroup(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Path metatables.
// ref: FUN_140606710
void ScriptClass_Path(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Rotation metatables.
// ref: FUN_140606760
void ScriptClass_Rotation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Scale metatables.
// ref: FUN_1406067b0
void ScriptClass_Scale(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the LineScale metatables.
// ref: FUN_140606670
void ScriptClass_LineScale(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Translation metatables.
// ref: FUN_140606800
void ScriptClass_Translation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the TextureCoordTranslation metatables.
// ref: FUN_140606850
void ScriptClass_TextureCoordTranslation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the LineTranslation metatables.
// ref: FUN_1406066c0
void ScriptClass_LineTranslation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Browser metatables.
// ref: FUN_1424a79e0
void ScriptClass_Browser(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the ColorSelect metatables.
// ref: FUN_1424ada10
void ScriptClass_ColorSelect(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the ControlPoint metatables.
// ref: FUN_1406065d0
void ScriptClass_ControlPoint(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the EditBox metatables.
// ref: FUN_1424b0730
void ScriptClass_EditBox(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Font metatables.
// ref: FUN_140a51310
void ScriptClass_Font(lua_State* L) {
    (void)L;
    CSimpleFont::CreateScriptMetaTables();
}

// the FontString metatables.
// ref: FUN_140a51320
void ScriptClass_FontString(lua_State* L) {
    (void)L;
    CSimpleFontString::CreateScriptMetaTables();
}

// the SimpleHTML metatables.
// ref: FUN_1424b8bd0
void ScriptClass_SimpleHTML(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Line metatables.
// ref: FUN_1424c4210
void ScriptClass_Line(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the MessageFrame metatables.
// ref: FUN_1424bb2d0
void ScriptClass_MessageFrame(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Model metatables.
// ref: FUN_1424bd400
void ScriptClass_Model(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the PlayerModelBase metatables.
// ref: FUN_141697780
void ScriptClass_PlayerModelBase(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the MovieFrame metatables.
// ref: FUN_14062e940
void ScriptClass_MovieFrame(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the OffScreenFrame metatables.
// ref: FUN_1424bfe70
void ScriptClass_OffScreenFrame(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the StatusBar metatables.
// ref: FUN_1424c2df0
void ScriptClass_StatusBar(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the PlayerModel metatables.
// ref: FUN_141a87070
void ScriptClass_PlayerModel(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the ModelSceneActor metatables.
// ref: FUN_141a95840
void ScriptClass_ModelSceneActor(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// C_Seasons (Lua::C_Seasons::GetActiveSeason, HasActiveSeason): Classic has no seasons here.
int Lua_C_Seasons_GetActiveSeason(lua_State* L) {
    FROST_UNIMPLEMENTED();
    lua_pushnil(L);
    return 1;
}

int Lua_C_Seasons_HasActiveSeason(lua_State* L) {
    FROST_UNIMPLEMENTED();
    lua_pushboolean(L, 0);
    return 1;
}

}

// ref: FUN_140afd040
void GlueRegisterScriptAPI(lua_State* L) {
    ScriptAPI_C_AccountServices(L);
    ScriptAPI_C_AddOns(L);
    ScriptRegister_140bca380(L);
    ScriptAPI_C_CameraDefaults(L);
    ScriptEnum_CharacterCreateRaceMode(L);
    ScriptGlobals_CanCreateCharacter(L);
    ScriptEnum_CharacterServiceInfoFlag(L);
    ScriptAPI_C_ChatInfo(L);
    ScriptRegister_140c891a0(L);
    ScriptEnum_ConfigurationWarning(L);
    ScriptEnum_ConsoleCategory(L);
    ScriptAPI_C_ConsoleScriptCollection(L);
    ScriptEnum_UICursorType(L);
    ScriptEnum_ManipulatorEventType(L);
    ScriptAPI_C_EventUtils(L);
    ScriptGlobals_ProcessExceptionClient(L);
    ScriptGlobals_CanUpgradeExpansion(L);
    ScriptClass_ModelScene(L);
    ScriptClass_Checkout(L);
    ScriptAPI_C_GamePad(L);
    ScriptAPI_C_GameRules(L);
    ScriptAPI_C_InterfaceFileManifest(L);
    ScriptEnum_BindingSet(L);
    ScriptAPI_Kiosk(L);
    ScriptRegister_140c1a3b0(L);
    ScriptRegister_141117760(L);
    ScriptAPI_C_LoginUI(L);
    ScriptAPI_C_PaidServices(L);
    ScriptEnum_ClientPlatformType(L);
    ScriptAPI_C_ReportSystem(L);
    ScriptSetCVarCallbacks_141168cf0();
    ScriptClass_Animation(L);
    ScriptClass_Alpha(L);
    ScriptClass_FlipBook(L);
    ScriptClass_AnimationGroup(L);
    ScriptClass_Path(L);
    ScriptClass_Rotation(L);
    ScriptClass_Scale(L);
    ScriptClass_LineScale(L);
    ScriptClass_Translation(L);
    ScriptClass_TextureCoordTranslation(L);
    ScriptClass_LineTranslation(L);
    ScriptClass_Browser(L);
    CSimpleButton::CreateScriptMetaTables();
    CSimpleCheckButton::CreateScriptMetaTables();
    ScriptClass_ColorSelect(L);
    ScriptClass_ControlPoint(L);
    ScriptClass_EditBox(L);
    ScriptClass_Font(L);
    ScriptClass_FontString(L);
    CSimpleFrame::CreateScriptMetaTables();
    ScriptClass_SimpleHTML(L);
    ScriptClass_Line(L);
    CSimpleMaskTexture::CreateScriptMetaTables();
    ScriptClass_MessageFrame(L);
    ScriptClass_Model(L);
    ScriptClass_PlayerModelBase(L);
    ScriptClass_MovieFrame(L);
    ScriptClass_OffScreenFrame(L);
    CSimpleScrollFrame::CreateScriptMetaTables();
    CSimpleSlider::CreateScriptMetaTables();
    ScriptClass_StatusBar(L);
    CSimpleTexture::CreateScriptMetaTables();
    ScriptAPI_C_SocialContractGlue(L);
    ScriptAPI_C_Sound(L);
    ScriptEnum_VasQueueStatus(L);
    ScriptAPI_C_StorePublic(L);
    ScriptEnum_BattlepayDisplayFlag(L);
    ScriptGlobals_StripHyperlinks(L);
    ScriptEnum_TitleIconVersion(L);
    ScriptAPI_C_UIColor(L);
    ScriptGlobals_DoesAnyDisplayHaveNotch(L);
    ScriptEnum_ModelSceneSetting(L);
    ScriptAPI_C_Timer(L);
    ScriptAPI_C_UserFeedback(L);
    ScriptEnum_GraphicsValidationResult(L);
    ScriptEnum_CommunicationMode(L);
    ScriptRegister_14115c010(L);
    ScriptAPI_C_XMLUtil(L);
    ScriptAPI_C_CVar(L);
    ScriptClass_PlayerModel(L);
    ScriptClass_ModelSceneActor(L);
    ScriptRegisterFunction(L, "C_Seasons", "GetActiveSeason", Lua_C_Seasons_GetActiveSeason);
    ScriptRegisterFunction(L, "C_Seasons", "HasActiveSeason", Lua_C_Seasons_HasActiveSeason);
    ScriptAPI_C_System(L);
    ScriptEnum_AccountData(L);
    ScriptEnum_CameraModeAspectRatio(L);
    ScriptEnum_ChrRacesAllianceType(L);
    ScriptEnum_CharCustomizationType(L);
    ScriptEnum_PermanentChatChannelType(L);
    ScriptEnum_ModelLightType(L);
    ScriptEnum_ModelBlendOperation(L);
    ScriptEnum_CraftingOrderDuration(L);
    ScriptEnum_ReportType(L);
    ScriptEnum_BattlepayBannerType(L);
    ScriptEnum_NodeOpFailureReason(L);
    ScriptEnum_UnitSex(L);
    ScriptEnum_ValueAddedServiceType(L);
    ScriptEnum_SeasonID(L);
}
