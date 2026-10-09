// Parses a small FrameXML document through XMLTree and BinaryXML, compacts it, and checks
// the records. Prints each failure; exits non-zero if any.

#include <cstdio>
#include <cstring>

#include "base/Status.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/XMLTree.hpp"

namespace {

int s_failures;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        s_failures++;
    }
}

const char s_document[] =
    "<Ui xmlns=\"http://www.blizzard.com/wow/ui/\">\n"
    "  <Script file=\"GlueParent.lua\"/>\n"
    "  <Include file=\"Fonts.xml\"/>\n"
    "  <Font name=\"GlueFontNormal\" font=\"Fonts\\FRIZQT__.TTF\" height=\"12\" outline=\"NORMAL\" virtual=\"true\">\n"
    "    <Color r=\"1.0\" g=\"0.82\" b=\"0\"/>\n"
    "    <Shadow><Offset x=\"1\" y=\"-1\"/><Color r=\"0\" g=\"0\" b=\"0\"/></Shadow>\n"
    "  </Font>\n"
    "  <Frame name=\"GlueParent\" setAllPoints=\"true\" frameStrata=\"LOW\" toplevel=\"true\">\n"
    "    <Size x=\"640\" y=\"480\"/>\n"
    "    <Anchors><Anchor point=\"TOPLEFT\" relativeTo=\"$parent\" relativePoint=\"BOTTOMLEFT\" x=\"10\" y=\"-4\"/></Anchors>\n"
    "    <Layers>\n"
    "      <Layer level=\"BACKGROUND\" textureSubLevel=\"12\">\n"
    "        <Texture name=\"$parentBackground\" file=\"Interface\\Glues\\Bg\" alphaMode=\"ADD\" hidden=\"true\">\n"
    "          <TexCoords left=\"0\" right=\"0.5\" top=\"0\" bottom=\"1\"/>\n"
    "        </Texture>\n"
    "        <FontString name=\"$parentText\" inherits=\"GlueFontNormal\" text=\"Hello\" maxLines=\"0x2\"/>\n"
    "      </Layer>\n"
    "    </Layers>\n"
    "    <Frames>\n"
    "      <Button name=\"$parentLogin\" text=\"LOGIN\">\n"
    "        <NormalTexture file=\"Interface\\Glues\\Button\"/>\n"
    "        <NormalFont style=\"GlueFontNormal\"/>\n"
    "      </Button>\n"
    "    </Frames>\n"
    "    <Scripts>\n"
    "      <OnLoad>self:RegisterEvent(\"FRAMES_LOADED\");</OnLoad>\n"
    "      <OnUpdate function=\"GlueParent_OnUpdate\"/>\n"
    "      <OnClick inherit=\"prepend\">print(1)</OnClick>\n"
    "    </Scripts>\n"
    "  </Frame>\n"
    "</Ui>\n";

XMLOBJECT* Child(XMLOBJECT* object, uint32_t index) {
    XMLOBJECT* child = object->m_child;
    while (child && index--) {
        child = child->m_next;
    }
    return child;
}

void CheckDocument(XMLOBJECT* ui) {
    Check(ui->m_type == XMLTYPE_UI, "root type");
    Check(strcmp(ui->m_tag.m_text, "Ui") == 0, "root tag");

    auto script = static_cast<XMLSCRIPT*>(Child(ui, 0));
    Check(script && script->m_type == XMLTYPE_SCRIPT, "script type");
    Check(script && (script->m_scriptFlags & 1) && strcmp(script->m_file.m_text, "GlueParent.lua") == 0, "script file");

    auto include = static_cast<XMLFILE*>(Child(ui, 1));
    Check(include && include->m_type == XMLTYPE_INCLUDE, "include type");
    Check(include && strcmp(include->m_file.m_text, "Fonts.xml") == 0, "include file");

    auto font = static_cast<XMLFONT*>(Child(ui, 2));
    Check(font && font->m_type == XMLTYPE_FONT, "font type");
    Check(font && strcmp(font->m_name.m_text, "GlueFontNormal") == 0, "font name");
    Check(font && (font->m_attributes & 0x210) == 0x210, "font virtual");
    Check(font && font->m_height == 12.0f && (font->m_fontFlags & 0x800), "font height");
    Check(font && strcmp(font->m_outline.m_text, "NORMAL") == 0, "font outline");
    auto color = font ? static_cast<XMLCOLOR*>(XMLObjectFindChild(font, XMLTYPE_COLOR)) : nullptr;
    uint32_t argb = 0;
    Check(color && XMLColorGet(color, &argb) && argb == 0xFFFFD100, "font color");
    auto shadow = font ? static_cast<XMLDIMENSION*>(XMLObjectFindChild(font, XMLTYPE_SHADOW)) : nullptr;
    Check(shadow != nullptr, "shadow");
    if (shadow) {
        auto offset = static_cast<XMLDIMENSION*>(XMLObjectFindChild(shadow, XMLTYPE_OFFSET));
        Check(offset && offset->m_x == 1.0f && offset->m_y == -1.0f, "shadow offset");
        Check(XMLObjectFindChild(shadow, XMLTYPE_COLOR) != nullptr, "shadow color");
    }

    auto frame = static_cast<XMLFRAME*>(Child(ui, 3));
    Check(frame && frame->m_type == XMLTYPE_FRAMEDEF && frame->m_frameType == XMLTYPE_FRAME, "frame type");
    if (!frame) {
        return;
    }
    Check(strcmp(frame->m_name.m_text, "GlueParent") == 0, "frame name");
    Check(frame->m_setAllPoints == 3, "frame setAllPoints");
    Check((frame->m_frameFlags & 0x4000) && frame->m_strata == 2, "frame strata");
    Check((frame->m_frameFlags & 0x300) == 0x300, "frame toplevel");
    Check(frame->m_id == -1, "frame id default");

    auto size = static_cast<XMLDIMENSION*>(XMLObjectFindChild(frame, XMLTYPE_SIZE));
    Check(size && size->m_x == 640.0f && size->m_y == 480.0f, "size");

    auto anchors = XMLObjectFindChild(frame, XMLTYPE_ANCHORS);
    auto anchor = anchors ? static_cast<XMLANCHOR*>(anchors->m_child) : nullptr;
    Check(anchor && anchor->m_type == XMLTYPE_ANCHOR, "anchor");
    if (anchor) {
        Check(anchor->m_point == 0 && anchor->m_relativePoint == 6, "anchor points");
        Check(strcmp(anchor->m_relativeTo.m_text, "$parent") == 0, "anchor relativeTo");
        Check(anchor->m_anchorFlags == 7, "anchor flags");
        Check(anchor->m_x == 10.0f && anchor->m_y == -4.0f, "anchor offset");
    }

    auto layers = XMLObjectFindChild(frame, XMLTYPE_LAYERS);
    auto layer = layers ? static_cast<XMLLAYER*>(layers->m_child) : nullptr;
    Check(layer && layer->m_type == XMLTYPE_LAYER && layer->m_level == 0 && layer->m_subLevel == 7, "layer");
    if (layer) {
        auto texture = static_cast<XMLTEXTURE*>(Child(layer, 0));
        Check(texture && texture->m_type == XMLTYPE_TEXTURE, "texture type");
        if (texture) {
            Check(strcmp(texture->m_file.m_text, "Interface\\Glues\\Bg") == 0, "texture file");
            Check(texture->m_alphaMode == 3, "texture alphaMode");
            Check((texture->m_regionFlags & 0x12) == 0x12, "texture hidden");
            auto coords = static_cast<XMLTEXCOORDS*>(XMLObjectFindChild(texture, XMLTYPE_TEXCOORDS));
            Check(coords && coords->m_right == 0.5f && coords->m_texFlags == 0xf, "texcoords");
        }
        auto text = static_cast<XMLFONTSTRING*>(Child(layer, 1));
        Check(text && text->m_type == XMLTYPE_FONTSTRING, "fontstring type");
        if (text) {
            Check(strcmp(text->m_text.m_text, "Hello") == 0, "fontstring text");
            Check(strcmp(text->m_inherits.m_text, "GlueFontNormal") == 0, "fontstring inherits");
            Check(text->m_maxLines == 2, "fontstring maxLines (hex)");
        }
    }

    auto frames = XMLObjectFindChild(frame, XMLTYPE_FRAMES);
    auto button = frames ? static_cast<XMLBUTTON*>(frames->m_child) : nullptr;
    Check(button && button->m_type == XMLTYPE_FRAMEDEF && button->m_frameType == XMLTYPE_BUTTON, "button");
    if (button) {
        Check(strcmp(button->m_text.m_text, "LOGIN") == 0, "button text");
        Check(XMLObjectFindChild(button, XMLTYPE_NORMALTEXTURE) != nullptr, "button normal texture");
        auto normalFont = static_cast<XMLFILE*>(XMLObjectFindChild(button, XMLTYPE_NORMALFONT));
        Check(normalFont && strcmp(normalFont->m_file.m_text, "GlueFontNormal") == 0, "button normal font");
    }

    auto scripts = XMLObjectFindChild(frame, XMLTYPE_SCRIPTS);
    Check(scripts != nullptr, "scripts");
    if (scripts) {
        auto onLoad = static_cast<XMLSCRIPTHANDLER*>(Child(scripts, 0));
        Check(onLoad && onLoad->m_type == 0x36 && strstr(onLoad->m_body.m_text, "FRAMES_LOADED"), "OnLoad");
        auto onUpdate = static_cast<XMLSCRIPTHANDLER*>(Child(scripts, 1));
        Check(onUpdate && onUpdate->m_type == 0x38 && (onUpdate->m_handlerFlags & 1)
            && strcmp(onUpdate->m_function.m_text, "GlueParent_OnUpdate") == 0, "OnUpdate function");
        auto onClick = static_cast<XMLSCRIPTHANDLER*>(Child(scripts, 2));
        Check(onClick && onClick->m_type == 0x1d && (onClick->m_handlerFlags & 0x800), "OnClick prepend");
    }
}

}

int main() {
    CStatus status;
    XMLTREE* tree = XMLTree_Load(s_document, sizeof(s_document) - 1, &status, "Test.xml");
    Check(tree != nullptr, "XMLTree_Load");
    if (!tree) {
        return 1;
    }

    XMLBUILDER builder = {};
    XMLOBJECT* ui = XMLObjectCreateDocument(XMLTree_GetRoot(tree), &builder);
    std::printf("-- built\n");
    CheckDocument(ui);

    uint64_t size = 0;
    XMLOBJECT* compiled = XMLObjectCompile(ui, &size);
    std::printf("-- compiled (%llu bytes)\n", static_cast<unsigned long long>(size));
    CheckDocument(compiled);
    XMLObjectFree(compiled, true);

    XMLTree_Free(tree);
    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "passed", s_failures);
    return s_failures ? 1 : 0;
}
