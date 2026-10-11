#include "ui/CSimpleMovieFrame.hpp"

#include <utility>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/OldMovie.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

const char CSimpleMovieFrame::s_queuedScriptTag[] = ".?AUQueuedScript@CSimpleMovieFrame@@";

int32_t CSimpleMovieFrame::s_metaTable = -2;
int32_t CSimpleMovieFrame::s_restrictedMetaTable = -2;
bool CSimpleMovieFrame::s_drawRegions;

namespace {

// The movie frames' pool (0x142e99cb8, made by the static initializer 0x1401df960).
CDataAllocator s_movieFrameHeap(0x350, 5);

// The scripts of a movie frame.
enum : uint8_t {
    SCRIPT_ONMOVIESHOWSUBTITLE = 0x31,
    SCRIPT_ONMOVIEHIDESUBTITLE = 0x32,
    SCRIPT_ONMOVIEFINISHED = 0x33,
};

// The strata whose frames draw the movie over their children (bits 7-10 of m_flags2).
constexpr uint32_t FRAME2_STRATA = 0x780;
constexpr uint32_t FRAME2_STRATA_TOP = 0x480;

// The batch's draw callback: the frame's renderer, while it has a player.
// ref: LAB_1406663b0
void RenderMovie(void* param, const float* view, const float* projection, void* frameBuffer) {
    (void)view;
    (void)projection;
    (void)frameBuffer;
    auto frame = static_cast<CSimpleMovieFrame*>(param);
    if (frame == nullptr || frame->m_player == nullptr || frame->m_renderFrame.m_invoke == nullptr) {
        return;
    }
    reinterpret_cast<void (*)(void*)>(frame->m_renderFrame.m_invoke)(frame->m_renderFrame.m_storage);
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_140662120
CSimpleMovieFrame::CSimpleMovieFrame(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_renderFrame.m_invoke = nullptr;
    this->m_renderFrame.m_manager = blz::function_empty_manager;
    this->m_player = nullptr;
    this->m_subtitlesEnabled = 0;
    this->m_329 = 0;
    this->m_32aDirtied = 0;
    this->m_runningQueued = 0;
}

// ref: FUN_140662770
CSimpleMovieFrame::~CSimpleMovieFrame() {
    this->DestroyPlayer();
    // FUN_1406626c0: the queue's strings and storage go with the vector
    this->m_queuedScripts.clear();
    this->m_renderFrame.m_manager(this->m_renderFrame.m_storage, nullptr, 0);
}

void* CSimpleMovieFrame::operator new(size_t size) {
    (void)size;
    return s_movieFrameHeap.GetData(false, ".?AVCSimpleMovieFrame@@", -2, 0x1f);
}

void CSimpleMovieFrame::operator delete(void* frame) {
    if (frame != nullptr) {
        s_movieFrameHeap.PutData(frame);
    }
}

// DAT_142e9a2b0
int32_t CSimpleMovieFrame::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_14062e940
void CSimpleMovieFrame::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleMovieFrame_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleMovieFrame_FillScriptMethods, true);
}

// ref: FUN_14062e920
void CSimpleMovieFrame::SetDrawRegions(bool draw) {
    s_drawRegions = draw;
    // FUN_1405efb70(DAT_142e979d8)
    CSimpleTop::s_instance->MarkStrataDirty();
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1406651a0
int32_t CSimpleMovieFrame::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_14062efd0
bool CSimpleMovieFrame::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONMOVIESHOWSUBTITLE) {
        data->m_wrapper = "return function(self,text) %s end";
        return true;
    }
    if (index == SCRIPT_ONMOVIEHIDESUBTITLE || index == SCRIPT_ONMOVIEFINISHED) {
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_140666160
bool CSimpleMovieFrame::IsObjectTypeName(const char* name) {
    return _stricmp(name, "MovieFrame") == 0 || this->CSimpleFrame::IsObjectTypeName(name);
}

// ref: FUN_1406660b0
bool CSimpleMovieFrame::IsObjectType(int32_t type) {
    if (type == CSimpleMovieFrame::GetObjectType() || type == CSimpleFrame::GetObjectType()) {
        return true;
    }
    return this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_140665190
const char* CSimpleMovieFrame::GetObjectTypeName() {
    return "MovieFrame";
}

// ref: FUN_140665180
uint32_t CSimpleMovieFrame::GetObjectSize() {
    return sizeof(CSimpleMovieFrame);
}

// ---- CSimpleFrame ----------------------------------------------------------------------------

// ref: FUN_140666230
void CSimpleMovieFrame::RunOnUpdate(float elapsed) {
    this->RunQueuedScripts();
    if (this->m_329 && !this->m_32aDirtied) {
        this->MarkBatchDirty();
        this->m_32aDirtied = 1;
    }
    this->CSimpleFrame::RunOnUpdate(elapsed);
    if (this->m_player != nullptr) {
        MoviePlayerUpdate(this->m_player);
    }
}

// ref: FUN_1406661b0
void CSimpleMovieFrame::AddRegionsToBatch(CRenderBatch* batch) {
    if (this->m_329 && s_drawRegions) {
        this->CSimpleFrame::AddRegionsToBatch(batch);
    }
    uint64_t drawOrder = this->GetDrawOrderKey(2, 0);
    batch->AddCallback(this, drawOrder, RenderMovie, this, (this->m_flags2 & FRAME2_STRATA) == FRAME2_STRATA_TOP);
}

// ---- the movie -------------------------------------------------------------------------------

// ref: FUN_14062e9e0
void CSimpleMovieFrame::DestroyPlayer() {
    CMoviePlayer* player = this->m_player;
    if (player != nullptr) {
        this->m_player = nullptr;
        MoviePlayerStop(player);
        MoviePlayerDestroy(player);
    }
}

// ref: FUN_14062ea20
void CSimpleMovieFrame::RunQueuedScripts() {
    if (this->m_runningQueued || this->m_queuedScripts.size() == 0) {
        return;
    }
    this->m_runningQueued = 1;
    // FUN_14062f070: the queue is taken whole, so scripts that queue more run next update
    blz::vector<QueuedScript, s_queuedScriptTag> queued(std::move(this->m_queuedScripts));
    for (uint64_t i = 0; i < queued.size(); i++) {
        QueuedScript& script = queued[i];
        if (script.m_type == QueuedScript::FINISHED) {
            this->RunScript(SCRIPT_ONMOVIEFINISHED, nullptr);
        } else if (script.m_type == QueuedScript::SHOW_SUBTITLE) {
            this->RunScript(SCRIPT_ONMOVIESHOWSUBTITLE, nullptr, static_cast<const char*>(script.m_text.m_data));
        } else if (script.m_type == QueuedScript::HIDE_SUBTITLE) {
            this->RunScript(SCRIPT_ONMOVIEHIDESUBTITLE, nullptr);
        }
    }
    this->m_runningQueued = 0;
}

// ref: FUN_14062eb70
void CSimpleMovieFrame::StartMovie(int32_t movieId, bool looping, bool* success, int32_t* returnCode) {
    this->m_queuedScripts.clear();
    this->DestroyPlayer();
    this->m_player = MoviePlayerCreate(this);
    int32_t result = MoviePlayerQueue(this->m_player, movieId, looping);
    *success = result == 0;
    *returnCode = result;
}

// ref: FUN_14062eb50
void CSimpleMovieFrame::StartMovieByName(const char* name, bool looping, int32_t resolution, bool* success,
    int32_t* returnCode) {
    (void)name;
    (void)looping;
    (void)resolution;
    *success = true;
    *returnCode = 0;
}

// ref: FUN_1406022d0
void CSimpleMovieFrame::QueueFinished() {
    QueuedScript& script = this->m_queuedScripts.emplace_back();
    script.m_type = QueuedScript::FINISHED;
}

// ref: FUN_14062ec50
void CSimpleMovieFrame::OnMovieFinished() {
    if (this->m_player == nullptr) {
        return;
    }
    this->DestroyPlayer();
    this->QueueFinished();
}

// ref: FUN_14062ee10
void CSimpleMovieFrame::StopMovie() {
    if (this->m_player != nullptr) {
        this->DestroyPlayer();
        this->QueueFinished();
    }
    this->RunQueuedScripts();
}

// ---- script methods --------------------------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleMovieFrame* MovieSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleMovieFrame*>(Self(L, CSimpleMovieFrame::GetObjectType(), index, usage));
}

// ref: FUN_140a41e50
int EnableSubtitles(lua_State* L) {
    const char* usage = "Usage: self:EnableSubtitles(enable)";
    int32_t index = 1;
    CSimpleMovieFrame* frame = MovieSelf(L, &index, usage);
    bool enable = RequiredBool(L, &index, usage);
    frame->m_subtitlesEnabled = enable;
    RaisePendingError(L, frame, "EnableSubtitles");
    return 0;
}

// ref: FUN_140a42c20
int StartMovie(lua_State* L) {
    const char* usage = "Usage: local success, returnCode = self:StartMovie(movieID [, looping])";
    int32_t index = 1;
    CSimpleMovieFrame* frame = MovieSelf(L, &index, usage);
    // FUN_1408c5050
    int32_t movieId = RequiredInt32(L, &index, usage);
    bool looping = false;
    if (lua_type(L, index) >= 1) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            looping = lua_toboolean(L, index) != 0;
        }
    }
    index++;
    bool success = false;
    int32_t returnCode = 0;
    frame->StartMovie(movieId, looping, &success, &returnCode);
    RaisePendingError(L, frame, "StartMovie");
    PushBool(L, success);
    lua_pushnumber(L, returnCode);
    return 2;
}

// ref: FUN_140a43ac0
int StartMovieByName(lua_State* L) {
    const char* usage = "Usage: local success, returnCode = self:StartMovieByName(movieName [, looping, resolution])";
    int32_t index = 1;
    CSimpleMovieFrame* frame = MovieSelf(L, &index, usage);
    const char* name = RequiredString(L, &index, usage);
    bool looping = false;
    if (lua_type(L, index) >= 1) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            looping = lua_toboolean(L, index) != 0;
        }
    }
    index++;
    int32_t resolution = 0;
    if (lua_type(L, index) >= 1) {
        resolution = RequiredInt32(L, &index, usage);
    }
    bool success = false;
    int32_t returnCode = 0;
    frame->StartMovieByName(name, looping, resolution, &success, &returnCode);
    RaisePendingError(L, frame, "StartMovieByName");
    PushBool(L, success);
    lua_pushnumber(L, returnCode);
    return 2;
}

// ref: FUN_140a44aa0
int StopMovie(lua_State* L) {
    int32_t index = 1;
    CSimpleMovieFrame* frame = MovieSelf(L, &index, "Usage: self:StopMovie()");
    frame->StopMovie();
    RaisePendingError(L, frame, "StopMovie");
    return 0;
}

// DAT_142cf9450
const luaL_Reg s_movieFrameMethods[] = {
    { "EnableSubtitles", EnableSubtitles },
    { "StartMovie", StartMovie },
    { "StartMovieByName", StartMovieByName },
    { "StopMovie", StopMovie },
};

}

// ref: FUN_140a511d0
void CSimpleMovieFrame_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_movieFrameMethods, 4);
}
