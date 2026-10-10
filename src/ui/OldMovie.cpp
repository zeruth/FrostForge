#include "ui/OldMovie.hpp"
#include "util/Unimplemented.hpp"

HSLOG g_movieLog;

// ref: FUN_1405fc880
CMoviePlayer* MoviePlayerCreate(CSimpleMovieFrame* frame) {
    (void)frame;
    SLogWrite(g_movieLog, "WowMoviePlayer CreateStream");
    FROST_UNIMPLEMENTED();  // the 0x3d0 stream: three decoder tracks (FUN_14122da80), its audio and its frame callback
    return nullptr;
}

// ref: FUN_1405fcc20
void MoviePlayerDestroy(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer DestroyStream");
    if (player != nullptr) {
        FROST_UNIMPLEMENTED();  // unlinks the stream from DAT_142e97a70 and frees its tracks
    }
}

namespace {

// ref: FUN_1405fec60
int32_t MoviePlayerOpen(CMoviePlayer* player, int32_t movieId, bool looping) {
    (void)player;
    (void)movieId;
    (void)looping;
    FROST_UNIMPLEMENTED();  // the Movie record, its MovieFileData per resolution, the DivX and audio tracks
    return 1;
}

}

// ref: FUN_1405feba0
int32_t MoviePlayerQueue(CMoviePlayer* player, int32_t movieId, bool looping) {
    int32_t result = MoviePlayerOpen(player, movieId, looping);
    const char* text;
    if (result == 0) {
        text = "Success";
    } else if (result == 1) {
        text = "Load DivX Failed";
    } else if (result == 2) {
        text = "Missing Rec";
    } else if (result == 3) {
        text = "Read File Failed";
    } else if (result == 4) {
        text = "Open Video Failed";
    } else {
        text = "Unknown Start Movie Result type";
    }
    SLogWrite(g_movieLog, "WowMoviePlayer QueueMovie ID: %d, Loop: %s, Result: %s", movieId,
        looping ? "Yes" : "No", text);
    return result;
}

// ref: FUN_140600a60
void MoviePlayerStop(CMoviePlayer* player) {
    SLogWrite(g_movieLog, "WowMoviePlayer StopMovie");
    if (player != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_140600a90: stops the tracks and the audio
    }
}

// ref: FUN_1405fcf30
void MoviePlayerUpdate(CMoviePlayer* player) {
    if (player != nullptr) {
        FROST_UNIMPLEMENTED();  // the stream's decoder (+0xa8), its slot 0x50
    }
}
