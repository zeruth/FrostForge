#ifndef GLUE_GLUE_MGR_HPP
#define GLUE_GLUE_MGR_HPP

// CGlueMgr (Classic/Source/Glue/CGlueMgr.cpp): the pre-world UI - login, realm list, character
// select - its UI root, its script API and the GlueXML it loads.

// The glue's start: frame types, the glue script API, then CGlueMgr::Initialize.
void GlueMgrStartup();

namespace CGlueMgr {
// Builds the glue UI root, a fresh Lua state with the glue API, and loads GlueXML.
void Initialize();
}

#endif
