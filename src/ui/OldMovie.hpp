#ifndef UI_OLD_MOVIE_HPP
#define UI_OLD_MOVIE_HPP

// Engine/Source/Frame/OldMovie: the movie player behind MovieFrame (WowMoviePlayer). A stream
// (0x3d0) decodes a Movie record's DivX video and its audio and hands frames to its movie
// frame's render callback. Only the entry points the frame calls are here; the decoder is not
// ported, so a movie fails to start with "Load DivX Failed".

#include <cstdint>

#include "storm/Log.hpp"

class CSimpleMovieFrame;
struct CMoviePlayer;

// The movie log, Logs\Movie.log (DAT_142e97a88).
extern HSLOG g_movieLog;

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

#endif
