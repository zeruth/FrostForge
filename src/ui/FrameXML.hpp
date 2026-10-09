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

// The engine's frame types: Button, CheckButton, EditBox, Frame, ...
bool FrameXML_RegisterDefault();
// The validateFrameXML CVar.
void FrameXML_RegisterCVars();

#endif
