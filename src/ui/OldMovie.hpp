#ifndef UI_OLD_MOVIE_HPP
#define UI_OLD_MOVIE_HPP

// Engine/Source/Frame/OldMovie: the movie player behind MovieFrame (IOldMoviePlayer). A movie
// handle (0x3d0) holds up to three encodings of the movie (480P, 720P, 1080P: Movie ->
// MovieVariation -> MovieFileData), each parsed by its own AVI parser and decoded by its own
// decoder (ui/MovieDecoder), plus the movie's audio, played by the sound engine or straight
// through FMOD, and its captions. The frame's render callback advances the movie: the audio's
// clock (or the wall clock) says which frame is due, the frames up to it are read from CAS and
// decoded, and the newest is drawn; a frame whose bytes are not on disk yet pauses the audio and
// shows the loading bar until they are.

#include <cstdint>

#include "storm/Log.hpp"

class CSimpleMovieFrame;
struct CMoviePlayer;

// The movie log, Logs\Movie.log (DAT_142e97a88).
extern HSLOG g_movieLog;  // untied: the port's own log handle

// The movie's variation for this window (MovieVariation record), or null.
const uint8_t* MovieGetVariation(int32_t movieId, bool localOnly);
// ref: FUN_1405fc880
CMoviePlayer* MoviePlayerCreate(CSimpleMovieFrame* frame);
// ref: FUN_1405fcc20
void MoviePlayerDestroy(CMoviePlayer* player);
// Queues movie `movieId`: 0 started, 1 the decoder failed to load, 2 no Movie record, 3 the
// file failed to read, 4 the video failed to open.
// ref: FUN_1405feba0
int32_t MoviePlayerQueue(CMoviePlayer* player, int32_t movieId, bool looping);
// ref: FUN_140600a60
void MoviePlayerStop(CMoviePlayer* player);
// ref: FUN_1405fcf30
void MoviePlayerUpdate(CMoviePlayer* player);
// The movie log and the audio device listeners (the client's startup), and their end.
// ref: FUN_1405fde10
void MoviePlayerInitialize();
// ref: FUN_1406006a0
void MoviePlayerShutdown();
// Whether the frames are drawn at all.
// ref: FUN_140600090
void MoviePlayerSetRenderEnabled(bool enabled);

#endif
