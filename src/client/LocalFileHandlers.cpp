#include "client/LocalFileHandlers.hpp"
#include "cas/CASLocalManifest.hpp"
#include "os/Os.hpp"

namespace {

// A loose image stands in for the .blp of the same name.
// ref: FUN_141281280
bool TextureLocalName(const char* extension, const char* path, char* dest, uint64_t size) {
    (void)extension;
    OsPathChangeExtension(dest, static_cast<uint32_t>(size), path, "blp");
    return true;
}

// A loose .wav stands in for the .ogg of the same name.
// ref: FUN_1412c79a0
bool SoundLocalName(const char* extension, const char* path, char* dest, uint64_t size) {
    (void)extension;
    OsPathChangeExtension(dest, static_cast<uint32_t>(size), path, "ogg");
    return true;
}

}

// ref: FUN_141280b60
void TextureRegisterLocalFileHandlers() {
    cas::LocalManifestRegisterHandler(&TextureLocalName, ".blp");
    cas::LocalManifestRegisterHandler(&TextureLocalName, ".tga");
    cas::LocalManifestRegisterHandler(&TextureLocalName, ".jpg");
    cas::LocalManifestRegisterHandler(&TextureLocalName, ".jpeg");
}

// ref: FUN_1412cc880
void SoundRegisterLocalFileHandlers() {
    cas::LocalManifestRegisterHandler(&SoundLocalName, ".wav");
}
