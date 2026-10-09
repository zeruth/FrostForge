#ifndef CLIENT_LOCAL_FILE_HANDLERS_HPP
#define CLIENT_LOCAL_FILE_HANDLERS_HPP

// The texture and sound modules' extension handlers for the CAS local manifest: a loose image
// or .wav under the install is entered under its archive name (.blp, .ogg).

void TextureRegisterLocalFileHandlers();
void SoundRegisterLocalFileHandlers();

#endif
