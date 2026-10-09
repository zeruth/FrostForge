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

// The engine's frame types: Button, CheckButton, EditBox, Frame, ...
bool FrameXML_RegisterDefault();
// The validateFrameXML CVar.
void FrameXML_RegisterCVars();

#endif
