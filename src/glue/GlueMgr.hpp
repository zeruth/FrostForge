#ifndef GLUE_GLUE_MGR_HPP
#define GLUE_GLUE_MGR_HPP

// CGlueMgr (Classic/Source/Glue/CGlueMgr.cpp): the pre-world UI - login, realm list, character
// select - its UI root, its script API and the GlueXML it loads.

// The glue's start: frame types, the glue script API, then CGlueMgr::Initialize.
void GlueMgrStartup();

// The device came back from a restart: the glue's next update resets the cursor
// (DAT_14311c52a, set at the end of FUN_140329450 and read by FUN_14167c520).
void GlueMgrNoteGxRestart();

namespace CGlueMgr {
// Builds the glue UI root, a fresh Lua state with the glue API, and loads GlueXML.
void Initialize();
}

#endif
