#include "ui/CSimpleCheckButton.hpp"

#include <cstring>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

int32_t CSimpleCheckButton::s_metaTable = -2;
int32_t CSimpleCheckButton::s_restrictedMetaTable = -2;

namespace {

// The check buttons' pool (0x1447aed58, made by the static initializer 0x14020db90).
CDataAllocator s_checkButtonHeap(0x380, 0x32);

// A new texture's image from a source that is not a texture object (FUN_1424ad600).
bool ApplyTextureSource(CSimpleTexture* texture, const TEXTURESOURCE* source) {
    if (source->GetTexture() != nullptr) {
        return false;
    }
    int32_t type = source->m_type;
    if (type == TEXTURESOURCE::SOURCE_FILE || type == TEXTURESOURCE::SOURCE_FILE_ID) {
        return texture->SetTextureById(source->GetFileDataId(), 0, 0, 1, true, false);
    }
    if (type == TEXTURESOURCE::SOURCE_ATLAS) {
        int32_t member = source->m_id;
        return texture->SetAtlas(&member, 1, false, 0, 0);
    }
    return false;
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424acbe0
CSimpleCheckButton::CSimpleCheckButton(CSimpleFrame* parent) : CSimpleButton(parent) {
    this->m_checked = false;
    this->m_checkedTexture = nullptr;
    this->m_disabledCheckedTexture = nullptr;
}

// ref: FUN_1424acc50
CSimpleCheckButton::~CSimpleCheckButton() = default;

void* CSimpleCheckButton::operator new(size_t size) {
    (void)size;
    return s_checkButtonHeap.GetData(false, ".?AVCSimpleCheckButton@@", -2, 0x1f);
}

void CSimpleCheckButton::operator delete(void* button) {
    if (button != nullptr) {
        s_checkButtonHeap.PutData(button);
    }
}

// DAT_14309fec4
int32_t CSimpleCheckButton::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424acce0
void CSimpleCheckButton::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleCheckButton_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleCheckButton_FillScriptMethods, true);
}

// ref: FUN_1424acd30
void CSimpleCheckButton::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424acda0
int32_t CSimpleCheckButton::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424acf30
bool CSimpleCheckButton::IsObjectTypeName(const char* name) {
    return _stricmp(name, "CheckButton") == 0 || _stricmp(name, "Button") == 0 || _stricmp(name, "Frame") == 0
        || _stricmp(name, "Region") == 0 || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424acdd0
bool CSimpleCheckButton::IsObjectType(int32_t type) {
    if (type == CSimpleCheckButton::GetObjectType() || type == CSimpleButton::GetObjectType()
        || type == CSimpleFrame::GetObjectType()) {
        return true;
    }
    return this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_1424acd90
const char* CSimpleCheckButton::GetObjectTypeName() {
    return "CheckButton";
}

// ref: FUN_1424acd80
uint32_t CSimpleCheckButton::GetObjectSize() {
    return 0x380;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424acfc0
void CSimpleCheckButton::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleButton::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x40)) {
        return;
    }
    auto xml = static_cast<XMLCHECKBUTTON*>(node);
    if (xml->m_checkFlags & 1) {
        this->SetChecked(((xml->m_checkFlags >> 1) & 1) != 0, false);
    }
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_CHECKEDTEXTURE) {
            auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
            TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
            this->SetCheckedTexture(&source);
        } else if (child->m_type == XMLTYPE_DISABLEDCHECKEDTEXTURE) {
            auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
            TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
            this->SetDisabledCheckedTexture(&source);
        }
    }
}

// ---- overrides ----------------------------------------------------------------------------------

// ref: FUN_1424ad6b0
void CSimpleCheckButton::RemoveRegion(CSimpleRegion* region) {
    if (region == this->m_checkedTexture) {
        this->m_checkedTexture = nullptr;
    }
    if (region == this->m_disabledCheckedTexture) {
        this->m_disabledCheckedTexture = nullptr;
    }
    this->CSimpleButton::RemoveRegion(region);
}

// ref: FUN_1424ad570
void CSimpleCheckButton::SetEnabled(bool enabled) {
    this->CSimpleButton::SetEnabled(enabled);
    this->SetChecked(this->m_checked, true);
}

// ref: FUN_1424ad090
void CSimpleCheckButton::OnClick(const char* button, bool down, bool fromInput, bool secure, const char* taint) {
    this->SetChecked(!this->m_checked, false);
    this->CSimpleButton::OnClick(button, down, fromInput, secure, taint);
}

// ---- the rest -----------------------------------------------------------------------------------

// ref: FUN_1424ad160
void CSimpleCheckButton::SetChecked(bool checked, bool force) {
    if (!force && checked == this->m_checked) {
        return;
    }
    this->m_checked = checked;
    if (this->m_checkedTexture != nullptr) {
        this->m_checkedTexture->Hide();
    }
    if (this->m_disabledCheckedTexture != nullptr) {
        this->m_disabledCheckedTexture->Hide();
    }
    if (!this->m_checked) {
        return;
    }
    if (this->m_disabledCheckedTexture != nullptr && (this->m_state & 0xf) == BUTTON_DISABLED) {
        this->m_disabledCheckedTexture->Show();
    } else if (this->m_checkedTexture != nullptr) {
        this->m_checkedTexture->Show();
    }
}

// ref: FUN_1424ad200
bool CSimpleCheckButton::SetCheckedTexture(const TEXTURESOURCE* source) {
    CSimpleTexture* adopted = source->GetTexture();
    CSimpleTexture* texture = this->m_checkedTexture;
    bool created = false;
    if (adopted == nullptr) {
        if (texture == nullptr) {
            texture = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
            this->m_checkedTexture = texture;
        }
        created = texture != nullptr;
    } else {
        if (texture == adopted) {
            return true;
        }
        if (texture != nullptr) {
            texture->Hide();
        }
        this->m_checkedTexture = adopted;
        texture = adopted;
    }
    bool result = true;
    if (texture != nullptr) {
        if (!texture->IsAnchored()) {
            texture->SetAllPoints(this, true);
        }
        texture->SetFrame(this, DRAWLAYER_OVERLAY, 0, false);
        if (created) {
            result = ApplyTextureSource(this->m_checkedTexture, source);
        }
    }
    this->SetChecked(this->m_checked, true);
    return result;
}

// ref: FUN_1424ad3c0
bool CSimpleCheckButton::SetDisabledCheckedTexture(const TEXTURESOURCE* source) {
    CSimpleTexture* adopted = source->GetTexture();
    CSimpleTexture* texture = this->m_disabledCheckedTexture;
    bool created = false;
    if (adopted == nullptr) {
        if (texture == nullptr) {
            texture = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
            this->m_disabledCheckedTexture = texture;
        }
        created = texture != nullptr;
    } else {
        // The reference compares against the checked texture here, not the disabled one.
        if (this->m_checkedTexture == adopted) {
            return true;
        }
        if (texture != nullptr) {
            texture->Hide();
        }
        this->m_disabledCheckedTexture = adopted;
        texture = adopted;
    }
    bool result = true;
    if (texture != nullptr) {
        if (!texture->IsAnchored()) {
            texture->SetAllPoints(this, true);
        }
        texture->SetFrame(this, DRAWLAYER_OVERLAY, 0, false);
        if (created) {
            result = ApplyTextureSource(this->m_disabledCheckedTexture, source);
        }
    }
    this->SetChecked(this->m_checked, true);
    return result;
}

// ---- script methods (table 0x142cf7ba0) ------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleCheckButton* CheckSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleCheckButton*>(Self(L, CSimpleCheckButton::GetObjectType(), index, usage));
}

// ref: FUN_1408f03c0
int GetChecked(lua_State* L) {
    int32_t index = 1;
    CSimpleCheckButton* button = CheckSelf(L, &index, "Usage: local checked = self:GetChecked()");
    bool checked = button->m_checked;
    RaisePendingError(L, button, "GetChecked");
    return PushBool(L, checked);
}

// ref: FUN_1408f1140
int GetCheckedTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleCheckButton* button = CheckSelf(L, &index, "Usage: local texture = self:GetCheckedTexture()");
    CSimpleTexture* texture = button->m_checkedTexture;
    RaisePendingError(L, button, "GetCheckedTexture");
    return PushObject(L, texture);
}

// ref: FUN_1408f1ec0
int GetDisabledCheckedTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleCheckButton* button = CheckSelf(L, &index, "Usage: local texture = self:GetDisabledCheckedTexture()");
    CSimpleTexture* texture = button->m_disabledCheckedTexture;
    RaisePendingError(L, button, "GetDisabledCheckedTexture");
    return PushObject(L, texture);
}

// ref: FUN_1408f2c40
int SetChecked(lua_State* L) {
    int32_t index = 1;
    CSimpleCheckButton* button = CheckSelf(L, &index, "Usage: self:SetChecked([checked])");
    bool checked = false;
    if (lua_type(L, index) >= 1) {
        checked = lua_toboolean(L, index) != 0;
    }
    button->SetChecked(checked, false);
    RaisePendingError(L, button, "SetChecked");
    return 0;
}

struct ASSETARGS {
    CSimpleCheckButton* m_button;
    TEXTURESOURCE m_source;
};

// ref: FUN_1408c7f90
int32_t SetCheckedTextureBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetCheckedTexture(asset)";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_button = CheckSelf(L, &index, usage);
    args->m_source = TextureSourceArgument(L, &index, usage);
    args->m_button->SetCheckedTexture(&args->m_source);
    RaisePendingError(L, args->m_button, "SetCheckedTexture");
    return 0;
}

// ref: FUN_1408c8c60
int32_t SetDisabledCheckedTextureBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetDisabledCheckedTexture(asset)";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_button = CheckSelf(L, &index, usage);
    args->m_source = TextureSourceArgument(L, &index, usage);
    args->m_button->SetDisabledCheckedTexture(&args->m_source);
    RaisePendingError(L, args->m_button, "SetDisabledCheckedTexture");
    return 0;
}

// ref: FUN_1408f3a00
int SetCheckedTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetCheckedTextureBody, &args);
}

// ref: FUN_1408f4720
int SetDisabledCheckedTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetDisabledCheckedTextureBody, &args);
}

// DAT_142cf7ba0
const luaL_Reg s_checkButtonMethods[] = {
    { "GetChecked", GetChecked },
    { "GetCheckedTexture", GetCheckedTexture },
    { "GetDisabledCheckedTexture", GetDisabledCheckedTexture },
    { "SetChecked", SetChecked },
    { "SetCheckedTexture", SetCheckedTexture },
    { "SetDisabledCheckedTexture", SetDisabledCheckedTexture },
};

}

// ref: FUN_140a50ec0
void CSimpleCheckButton_FillScriptMethods(lua_State* L) {
    CSimpleButton_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_checkButtonMethods, 6);
}
