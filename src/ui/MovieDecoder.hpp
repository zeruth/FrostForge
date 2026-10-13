#ifndef UI_MOVIE_DECODER_HPP
#define UI_MOVIE_DECODER_HPP

// Engine/Source/MoviePlayerOld: the cinematics' container and video decoders (namespace
// movieplayerv1). A movie is an AVI whose video track is VP9 (or AV1); the parser reads its
// headers and index, and splits the file into CAS-sized blocks so the player can see whether a
// frame's bytes are on disk before it reads them. The decoder turns a frame's bytes into three
// plane textures (Y, U, V) and draws them as a quad through the "CVideoDecoderVPX" pipeline,
// which converts to RGB.

#include <cstdint>

#include "blz/Vector.hpp"

namespace cas {
struct CASFILE;
}


namespace movieplayerv1 {

extern const char s_indexTag[];
extern const char s_tactBlockTag[];

// One idx1 entry (0x10): the chunk, its flags (0x10 keyframe), its offset from the movi list's
// type field and its size.
struct AVIINDEXENTRY {
    uint32_t m_chunkId;             // 0x00
    uint32_t m_flags;               // 0x04
    uint32_t m_offset;              // 0x08
    uint32_t m_size;                // 0x0C
};
static_assert(sizeof(AVIINDEXENTRY) == 0x10, "AVIINDEXENTRY is 0x10 bytes");

// A piece of the file as CAS streams it (8 bytes).
struct STactBlock {
    uint32_t m_offset;
    uint32_t m_size;
};
static_assert(sizeof(STactBlock) == 8, "STactBlock is 8 bytes");

// One file of a movie (0x98).
class CAviParser {
    public:
    int32_t m_fileDataId;                                   // 0x00
    uint32_t m_pad04;
    void* m_pviv;                                           // 0x08 the video key chunk ('pviv'), when encrypted
    void* m_paiv;                                           // 0x10 the audio key chunk ('paiv')
    uint32_t m_fourcc;                                      // 0x18 the video's compression (strf)
    uint32_t m_pad1c;
    uint64_t m_moviOffset;                                  // 0x20 the movi list's type field
    blz::vector<AVIINDEXENTRY, s_indexTag> m_index;         // 0x28
    blz::vector<STactBlock, s_tactBlockTag> m_blocks;       // 0x40
    float m_fps;                                            // 0x58
    int32_t m_width;                                        // 0x5C
    int32_t m_height;                                       // 0x60
    int32_t m_frameCount;                                   // 0x64 (strh length)
    int32_t m_videoKBps;                                    // 0x68
    int32_t m_audioKBps;                                    // 0x6C
    int32_t m_totalKBps;                                    // 0x70
    int32_t m_seconds;                                      // 0x74
    cas::CASFILE* m_file;                                   // 0x78
    uint32_t m_videoBytes;                                  // 0x80
    uint32_t m_videoChunks;                                 // 0x84
    uint32_t m_maxFrameSize;                                // 0x88
    uint32_t m_audioBytes;                                  // 0x8C
    uint32_t m_audioChunks;                                 // 0x90
    uint32_t m_pad94;

    // ref: FUN_14122da80
    CAviParser();
    // ref: FUN_14122dae0
    ~CAviParser();
    // The file and the key chunks let go.
    // ref: FUN_14122db40
    void Close();
    // ref: FUN_14122dbc0
    void Reset();
    // ref: FUN_14122dc10
    void SetFile(int32_t fileDataId);
    // The file opened and parsed, with its totals and blocks; false when it is not a movie.
    // ref: FUN_14122dc70
    bool Open();

    private:
    // ref: FUN_14122ddf0
    bool ReadHeaders();
    // ref: FUN_14122e300
    bool BuildBlocks();
};
static_assert(sizeof(CAviParser) == 0x98, "CAviParser is 0x98 bytes");

// The decoders' interface (vtable 0x1427232e0).
class IVideoDecoder {
    public:
    // ref: FUN_14122f5f0
    IVideoDecoder();
    // ref: FUN_14122f660
    virtual ~IVideoDecoder();                                    // 0x00 (the reference's slot 0x30)
    // The largest picture its textures hold (slot 0x00).
    virtual void GetMaxSize(float* size) = 0;
    // Codec and device objects made (slot 0x08).
    virtual bool Initialize() = 0;
    // A frame decoded (slot 0x10); the frame's bytes only run through the key stream (slot 0x18).
    virtual bool Decode(const void* data, uint32_t size) = 0;
    virtual bool Skip(const void* data, uint32_t size) = 0;
    // ref: (slot 0x20)
    virtual void Shutdown() = 0;
    // The decoded picture uploaded and its pipeline and textures bound (slot 0x28).
    virtual void Bind() = 0;
    // The decryption key and IV (slot 0x60).
    virtual void SetKey(const void* key, const void* iv) = 0;
    // The codec's error (slot 0x68).
    virtual void GetError(char* buffer, uint32_t size) = 0;
    // The device's objects (slots 0x70 and 0x78), and the codec (slot 0x80).
    virtual bool CreateDeviceObjects() = 0;
    virtual bool DestroyDeviceObjects() = 0;
    virtual bool InitCodec() = 0;

    // The picture's size; Initialize says whether it worked.
    // ref: FUN_14122fc30
    bool SetSize(int32_t width, int32_t height);
    // ref: FUN_14122fc50
    void Close();
    // ref: FUN_14122f790
    bool DecodeFrame(const void* data, uint32_t size);
    // The picture drawn into the rectangle (in UI units), letterboxed to its own shape.
    // ref: FUN_14122f7a0
    void Render(const float* rect);

    int32_t m_width = 0;                // 0x08
    int32_t m_height = 0;               // 0x0C
    uint8_t m_ready = 0;                // 0x10
};

// The video decoder for a FourCC: VP90 and VP9P get libvpx, AV01 dav1d (not ported).
// ref: FUN_14122f690
IVideoDecoder* VideoDecoderCreate(uint32_t fourcc);

}  // namespace movieplayerv1

#endif
