#ifndef UI_C_SIMPLE_MOVIE_FRAME_HPP
#define UI_C_SIMPLE_MOVIE_FRAME_HPP

// Engine/Source/Frame/CSimpleMovieFrame ("MovieFrame", 0x350 bytes; vtable 0x1426ce9b8,
// CLayoutFrame vtable 0x1426cecd0 at +0x18, CAnimatableObject vtable 0x1426cedc8 at +0x1d8): a
// frame that plays a movie through the OldMovie player and draws its frames with a render
// callback. What the player reports (the movie ending, a subtitle shown or hidden) is queued and
// run as scripts on the next update.

#include <cstdint>

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "blz/Vector.hpp"
#include "ui/CSimpleFrame.hpp"

struct CMoviePlayer;
struct lua_State;

class CSimpleMovieFrame : public CSimpleFrame {
    public:
    // A player report waiting to run as a script (CSimpleMovieFrame::QueuedScript, 0x30).
    struct QueuedScript {
        enum : int32_t {
            FINISHED = 1,       // OnMovieFinished
            SHOW_SUBTITLE = 2,  // OnMovieShowSubtitle(text)
            HIDE_SUBTITLE = 3,  // OnMovieHideSubtitle
        };
        int32_t m_type;
        int32_t m_pad04;
        blz::string m_text;
    };
    static const char s_queuedScriptTag[];

    blz::function_storage m_renderFrame;    // 0x300 draws the current movie frame
    CMoviePlayer* m_player;                 // 0x320
    uint8_t m_subtitlesEnabled;             // 0x328
    uint8_t m_329;                          // 0x329 never set by this class
    uint8_t m_32aDirtied;                   // 0x32a
    uint8_t m_pad32b[5];
    blz::vector<QueuedScript, s_queuedScriptTag> m_queuedScripts;  // 0x330
    uint8_t m_runningQueued;                // 0x348
    uint8_t m_pad349[7];

    // ref: FUN_140662120 (the construction half)
    explicit CSimpleMovieFrame(CSimpleFrame* parent);
    // ref: FUN_140662770
    ~CSimpleMovieFrame() override;
    // Movie frames come from their own pool (0x142e99cb8, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id (DAT_142e9a2b0).
    static int32_t GetObjectType();
    // ref: FUN_14062e940
    static void CreateScriptMetaTables();
    // The metatables (DAT_142cf54b4, DAT_142cf54b8).
    static int32_t s_metaTable;
    static int32_t s_restrictedMetaTable;
    // Whether movie frames add their own regions to the batch (DAT_142cf54bc).
    static bool s_drawRegions;
    // ref: FUN_14062e920
    static void SetDrawRegions(bool draw);

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void RunOnUpdate(float elapsed) override;
    void AddRegionsToBatch(CRenderBatch* batch) override;

    // ref: FUN_14062e9e0
    void DestroyPlayer();
    // ref: FUN_14062ea20
    void RunQueuedScripts();
    // Starts movie `movieId`, replacing any playing one.
    // ref: FUN_14062eb70
    void StartMovie(int32_t movieId, bool looping, bool* success, int32_t* returnCode);
    // By name: the reference only reports success.
    // ref: FUN_14062eb50
    void StartMovieByName(const char* name, bool looping, int32_t resolution, bool* success, int32_t* returnCode);
    // The player finished the movie: drop it and queue OnMovieFinished.
    // ref: FUN_14062ec50
    void OnMovieFinished();
    // ref: FUN_14062ee10
    void StopMovie();

    private:
    void QueueFinished();
};

static_assert(sizeof(CSimpleMovieFrame) == 0x350, "CSimpleMovieFrame is 0x350 bytes");

void CSimpleMovieFrame_FillScriptMethods(lua_State* L);

#endif
