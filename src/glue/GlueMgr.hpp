#ifndef GLUE_GLUE_MGR_HPP
#define GLUE_GLUE_MGR_HPP

// CGlueMgr (Classic/Source/Glue/CGlueMgr.cpp): the pre-world UI - login, realm list, character
// select - its UI root, its script API and the GlueXML it loads.

#include <cstdint>

// The glue's start: frame types, the glue script API, then CGlueMgr::Initialize.
void GlueMgrStartup();

// The device came back from a restart: the glue's next update resets the cursor
// (DAT_14311c52a, set at the end of FUN_140329450 and read by FUN_14167c520).
void GlueMgrNoteGxRestart();

namespace CGlueMgr {
// Builds the glue UI root, a fresh Lua state with the glue API, and loads GlueXML.
void Initialize();
}

// The cursor module (Interface\Cursor): its mode (FUN_141cb20e0), the size index from
// cursorSizePreferred (FUN_14167f5c0), and its textures dropped and loaded again (FUN_141cb1cf0).
void CursorSetMode(uint32_t mode);
void CursorSetSize(int32_t index);
void CursorUnload();

// The Login button's start (FUN_1416a0db0, translated).
void GlueLoginStart(int32_t a1);

#endif
