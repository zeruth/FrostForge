#ifndef UI_FRAME_XML_HPP
#define UI_FRAME_XML_HPP

// FrameXML: building UI frames from .toc/.xml files. This part holds the frame types the
// parser can create (a factory per XML element name) and the parser's CVars.

#include <cstdint>

class CSimpleFrame;

// Creates a frame of one type under `parent`.
typedef CSimpleFrame* (*FRAMEFACTORY)(CSimpleFrame* parent);

// Adds a frame type; false if the name (case-insensitive) is already taken.
bool FrameXML_RegisterFactory(const char* type, FRAMEFACTORY factory, bool unknown);
// The factory for a type name, or null.
FRAMEFACTORY FrameXML_GetFactory(const char* type);

struct XMLOBJECT;

// The compiled intrinsic element of a name, marked in use; *recursive when it already was.
// Each acquire is paired with FrameXML_ReleaseIntrinsic.
XMLOBJECT* FrameXML_AcquireIntrinsic(const char* name, const char** taint, bool* recursive);
void FrameXML_ReleaseIntrinsic(const char* name);

class CScriptStatus;
class XMLNode;
struct SCRIPTLOCATION;

// ---- loading ---------------------------------------------------------------------------

// Called after each TOC entry with the fraction loaded.
typedef void (*FRAMEXMLPROGRESS)(float fraction, void* context);

// Loads a .toc and every file it lists. `localFiles` lets local overrides replace archive
// files; `arg` and `flags` are what each Lua file runs with (see FrameScript_ExecuteBuffer).
bool FrameXML_LoadToc(const char* toc, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags);
int32_t FrameXML_CountTocFiles(const char* text);
bool FrameXML_LoadFile(const char* path, const char* dir, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags);
bool FrameXML_LoadXML(const char* path, const char* dir, const char* arg, CScriptStatus* status, bool localFiles, uint32_t flags);
void FrameXML_Walk(const char* path, const char* dir, const char* arg, CScriptStatus* status, XMLOBJECT* ui, bool localFiles, uint32_t flags);
void FrameXML_Validate(XMLNode* node, CScriptStatus* status);
void FrameXML_BuildPath(char* dest, uint64_t size, const char* path, const char* dir, const char* file, bool localFiles);
CSimpleFrame* FrameXML_CreateFrame(XMLOBJECT* object, CSimpleFrame* parent, CScriptStatus* status);

void FrameXML_SetProgress(FRAMEXMLPROGRESS progress, void* context, uint32_t total);
bool FrameXML_IsLoadingToc();
bool FrameXML_GetLocalFiles();
int32_t FrameXML_GetDebugLevel();
void FrameXML_SetDebugLevel(int32_t level);
// The scoped-modifier flags new elements are stamped with (1 forbidden, 4 scripts use the
// given environment, 8 full lockdown, 0x10 hidden from the global environment), less any
// the mask forbids.
uint32_t FrameXML_GetScopedState();
void FrameXML_SetScopedState(uint32_t state);
void FrameXML_AddScopedMask(uint32_t mask);
void FrameXML_ClearScopedMask();

void XMLStatusSetLocation(CScriptStatus* status, XMLOBJECT* object);
void XMLStatusRestoreLocation(CScriptStatus* status, const SCRIPTLOCATION* location);

// ---- virtuals and intrinsics -----------------------------------------------------------

bool FrameXML_AddDeferredNode(bool intrinsic, XMLOBJECT* object, const char* name, const char* taint, CScriptStatus* status);
bool FrameXML_HasVirtual(const char* name);
XMLOBJECT* FrameXML_AcquireVirtual(const char* name, const char** taint, bool* busy);
void FrameXML_ReleaseVirtual(const char* name);
const char* FrameXML_GetInheritedParent(XMLOBJECT* object);
void FrameXML_FreeVirtuals();
void FrameXML_FreeIntrinsics();
void FrameXML_FreeFactories();

// The engine's frame types: Button, CheckButton, EditBox, Frame, ...
bool FrameXML_RegisterDefault();
// The validateFrameXML CVar.
void FrameXML_RegisterCVars();

#endif
