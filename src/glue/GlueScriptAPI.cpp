// The glue's script API (FUN_140afd040): the namespaces (C_*), the Enum tables, the globals and
// the class metatables, registered into the glue's Lua state in the reference's order. Each
// registration not yet ported is a stub named for what it registers.

#include "glue/GlueScriptAPI.hpp"

#include "framescript/ScriptAPI.hpp"
#include "glue/ScriptAPIRegistry.hpp"

#include "ui/CSimpleAnim.hpp"
#include "ui/CSimpleButton.hpp"
#include "ui/CSimpleModel.hpp"
#include "ui/CSimpleModelFFX.hpp"
#include "ui/CSimpleMovieFrame.hpp"
#include "ui/CSimpleEditBox.hpp"
#include "ui/CSimpleHTML.hpp"
#include "ui/CSimpleSlider.hpp"
#include "ui/CSimpleStatusBar.hpp"
#include "ui/CSimpleScrollFrame.hpp"
#include "ui/CSimpleCheckButton.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/ModelSceneFrame.hpp"
#include "util/Unimplemented.hpp"

// The ModelSceneActor methods after the script object ones (src/ui/ModelSceneActorScript.cpp).
void ModelSceneActor_FillScriptMethods(lua_State* L);

extern "C" {
#include "lua.h"
}

namespace {

int32_t s_modelSceneActorMetaTable = -2;  // DAT_142d2f560
int32_t s_modelSceneActorRestrictedMetaTable = -2;  // DAT_142d2f564

// the ModelScene metatables (ModelSceneFrame::CreateScriptMetaTables, FUN_141a96680).
void ScriptClass_ModelScene(lua_State* L) {
    (void)L;
    ModelSceneFrame::CreateScriptMetaTables();
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

// the OffScreenFrame metatables.
// ref: FUN_1424bfe70
void ScriptClass_OffScreenFrame(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the PlayerModel metatables.
// ref: FUN_141a87070
void ScriptClass_PlayerModel(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// the ModelSceneActor metatables: the actors are the reference's own objects, made by the
// translated ModelScene methods, which read these (shared at the reference's addresses).
// ref: FUN_141a95840
void ScriptClass_ModelSceneActor(lua_State* L) {
    s_modelSceneActorMetaTable = FrameScript_Object::CreateScriptMetaTable(L, ModelSceneActor_FillScriptMethods, false);
    s_modelSceneActorRestrictedMetaTable =
        FrameScript_Object::CreateScriptMetaTable(L, ModelSceneActor_FillScriptMethods, true);
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
    CSimpleAnim::CreateScriptMetaTables();
    CSimpleAlphaAnim::CreateScriptMetaTables();
    CSimpleFlipBookAnim::CreateScriptMetaTables();
    CSimpleAnimGroup::CreateScriptMetaTables();
    CSimpleAnimPath::CreateScriptMetaTables();
    CSimpleRotationAnim::CreateScriptMetaTables();
    CSimpleScaleAnim::CreateScriptMetaTables();
    CSimpleLineScaleAnim::CreateScriptMetaTables();
    CSimpleTexCoordTranslationAnim::CreateScriptMetaTables();
    CSimpleTranslationAnim::CreateScriptMetaTables();
    CSimpleLineTranslationAnim::CreateScriptMetaTables();
    ScriptClass_Browser(L);
    CSimpleButton::CreateScriptMetaTables();
    CSimpleCheckButton::CreateScriptMetaTables();
    ScriptClass_ColorSelect(L);
    CSimpleControlPoint::CreateScriptMetaTables();
    CSimpleEditBox::CreateScriptMetaTables();
    ScriptClass_Font(L);
    ScriptClass_FontString(L);
    CSimpleFrame::CreateScriptMetaTables();
    CSimpleHTML::CreateScriptMetaTables();
    ScriptClass_Line(L);
    CSimpleMaskTexture::CreateScriptMetaTables();
    ScriptClass_MessageFrame(L);
    CSimpleModel::CreateScriptMetaTables();
    CSimpleModelFFX::CreateScriptMetaTables();
    CSimpleMovieFrame::CreateScriptMetaTables();
    ScriptClass_OffScreenFrame(L);
    CSimpleScrollFrame::CreateScriptMetaTables();
    CSimpleSlider::CreateScriptMetaTables();
    CSimpleStatusBar::CreateScriptMetaTables();
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

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142d2f560 = (void*)&s_modelSceneActorMetaTable;
extern "C" void* const gen_p_142d2f564 = (void*)&s_modelSceneActorRestrictedMetaTable;
// ---- end of generated globals ----
