// The glue's script API (FUN_140afd040): the namespaces (C_*), the Enum tables, the globals and
// the class metatables, registered into the glue's Lua state in the reference's order. Each
// registration not yet ported is a stub named for what it registers.

#include "glue/GlueScriptAPI.hpp"

#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTexture.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lua.h"
}

namespace {

// ref: FUN_1407d53f0
void ScriptAPI_C_AccountServices(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1407d60e0
void ScriptAPI_C_AddOns(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140bca380
void ScriptRegister_140bca380(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c846d0
void ScriptAPI_C_CameraDefaults(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c84910
void ScriptEnum_CharacterCreateRaceMode(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c869e0
void ScriptGlobals_CanCreateCharacter(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c86c60
void ScriptEnum_CharacterServiceInfoFlag(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c88ac0
void ScriptAPI_C_ChatInfo(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c891a0
void ScriptRegister_140c891a0(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d959d0
void ScriptEnum_ConfigurationWarning(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d95f90
void ScriptEnum_ConsoleCategory(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d95ef0
void ScriptAPI_C_ConsoleScriptCollection(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140f8f150
void ScriptEnum_UICursorType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140f922a0
void ScriptEnum_ManipulatorEventType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14107ea90
void ScriptAPI_C_EventUtils(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14107eb00
void ScriptGlobals_ProcessExceptionClient(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14107ecd0
void ScriptGlobals_CanUpgradeExpansion(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

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

// ref: FUN_140f2cb90
void ScriptAPI_C_GamePad(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140f2cea0
void ScriptAPI_C_GameRules(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c132d0
void ScriptAPI_C_InterfaceFileManifest(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c19f30
void ScriptEnum_BindingSet(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c1a2e0
void ScriptAPI_Kiosk(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c1a3b0
void ScriptRegister_140c1a3b0(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141117760
void ScriptRegister_141117760(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141117780
void ScriptAPI_C_LoginUI(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1410a7e00
void ScriptAPI_C_PaidServices(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140ec54a0
void ScriptEnum_ClientPlatformType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b012b0
void ScriptAPI_C_ReportSystem(lua_State* L) {
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

// the Button metatables.
// ref: FUN_1424aafd0
void ScriptClass_Button(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the CheckButton metatables.
// ref: FUN_1424acce0
void ScriptClass_CheckButton(lua_State* L) {
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

// the ScrollFrame metatables.
// ref: FUN_1424c0d80
void ScriptClass_ScrollFrame(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the Slider metatables.
// ref: FUN_1424c1c50
void ScriptClass_Slider(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the StatusBar metatables.
// ref: FUN_1424c2df0
void ScriptClass_StatusBar(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140ae5d90
void ScriptAPI_C_SocialContractGlue(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140ae6420
void ScriptAPI_C_Sound(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e6b6e0
void ScriptEnum_VasQueueStatus(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e6bee0
void ScriptAPI_C_StorePublic(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e6e450
void ScriptEnum_BattlepayDisplayFlag(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e6f870
void ScriptGlobals_StripHyperlinks(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e701d0
void ScriptEnum_TitleIconVersion(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d276b0
void ScriptAPI_C_UIColor(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d277d0
void ScriptGlobals_DoesAnyDisplayHaveNotch(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d278a0
void ScriptEnum_ModelSceneSetting(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d286f0
void ScriptAPI_C_Timer(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b5f920
void ScriptAPI_C_UserFeedback(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b60950
void ScriptEnum_GraphicsValidationResult(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b623c0
void ScriptEnum_CommunicationMode(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14115c010
void ScriptRegister_14115c010(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14115c0c0
void ScriptAPI_C_XMLUtil(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c80980
void ScriptAPI_C_CVar(lua_State* L) {
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

// ref: FUN_140e6f9b0
void ScriptAPI_C_System(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1407d3000
void ScriptEnum_AccountData(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c84440
void ScriptEnum_CameraModeAspectRatio(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c84710
void ScriptEnum_ChrRacesAllianceType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c85a00
void ScriptEnum_CharCustomizationType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140c86fe0
void ScriptEnum_PermanentChatChannelType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14107eee0
void ScriptEnum_ModelLightType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1410a73a0
void ScriptEnum_ModelBlendOperation(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140ec7fc0
void ScriptEnum_CraftingOrderDuration(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140affd60
void ScriptEnum_ReportType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140e6bf80
void ScriptEnum_BattlepayBannerType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140d0ad60
void ScriptEnum_NodeOpFailureReason(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b5dfb0
void ScriptEnum_UnitSex(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140b605c0
void ScriptEnum_ValueAddedServiceType(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141088c70
void ScriptEnum_SeasonID(lua_State* L) {
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

// A function in a namespace table, made when missing (FUN_1407d2f20).
// ref: FUN_1407d2f20
void ScriptRegisterFunction(lua_State* L, const char* space, const char* name, lua_CFunction function) {
    lua_getfield(L, LUA_GLOBALSINDEX, space);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        lua_createtable(L, 0, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_GLOBALSINDEX, space);
    }
    lua_pushcclosure(L, function, 0);
    lua_setfield(L, -2, name);
    lua_settop(L, -2);
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
    ScriptClass_Button(L);
    ScriptClass_CheckButton(L);
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
    ScriptClass_ScrollFrame(L);
    ScriptClass_Slider(L);
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
