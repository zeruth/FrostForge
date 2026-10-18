#include "ui/MovieDecoder.hpp"

#include <cstdio>
#include <cstring>

#include <malloc.h>

#include "base/Coordinate.hpp"
#include "cas/CAS.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "storm/Memory.hpp"
#include "ui/CRenderBatch.hpp"
#include "util/Unimplemented.hpp"

// (libvpx's headers define a static typed wrapper per control id; the unused ones are C4505)
#pragma warning(push)
#pragma warning(disable : 4505)
extern "C" {
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
}
#pragma warning(pop)

extern GxRenderContext* g_gxRenderContext;  // DAT_14348b658

namespace movieplayerv1 {

const char s_indexTag[] = ".?AUAVIINDEXENTRY@CAviParser@movieplayerv1@@";
const char s_tactBlockTag[] = ".?AUSTactBlock@CAviParser@movieplayerv1@@";

namespace {

const char s_aviParserFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\MoviePlayerOld\\AviParser.cpp";
const char s_decoderFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\MoviePlayerOld\\IVideoDecoder.cpp";
const char s_blockTag[] = ".?AUBlock@movieplayerv1@@";
const char s_byteTag[] = ".E";

constexpr uint32_t FOURCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8
        | static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

// A run of the file of one kind (0x10): 'z' bytes CAS reads as they come, 'n' the movi data.
struct BLOCK {
    uint64_t m_size;
    uint8_t m_kind;
    uint8_t m_pad09[7];
};

// The blocks, split at 512KB (0x80000).
struct BLOCKLIST {
    blz::vector<BLOCK, s_blockTag> m_blocks;
    uint32_t m_maxSize = 0x80000;

    void Push(uint64_t size, uint8_t kind) {
        BLOCK block = {};
        block.m_size = size;
        block.m_kind = kind;
        this->m_blocks.push_back(block);
    }

    // `size` more bytes of `kind`: onto the last block when it is of the kind and has room,
    // otherwise in new blocks of at most the maximum.
    // ref: FUN_14122f020
    void Append(uint8_t kind, uint64_t size) {
        if (this->m_blocks.size() == 0) {
            this->Push(size, kind);
            return;
        }
        BLOCK& last = this->m_blocks[this->m_blocks.size() - 1];
        if (last.m_kind == kind && last.m_size + size <= this->m_maxSize) {
            last.m_size += size;
            return;
        }
        while (size != 0) {
            uint64_t take = size < this->m_maxSize ? size : this->m_maxSize;
            this->Push(take, kind);
            size -= take;
        }
    }
};

}  // namespace

// ---- CAviParser ------------------------------------------------------------------------------

// ref: FUN_14122da80
CAviParser::CAviParser() {
    this->m_fileDataId = 0;
    this->m_pad04 = 0;
    this->m_pviv = nullptr;
    this->m_paiv = nullptr;
    this->m_fourcc = 0;
    this->m_pad1c = 0;
    this->m_moviOffset = 0;
    this->m_fps = 0.0f;
    this->m_width = 0;
    this->m_height = 0;
    this->m_frameCount = 0;
    this->m_videoKBps = 0;
    this->m_audioKBps = 0;
    this->m_totalKBps = 0;
    this->m_seconds = 0;
    this->m_file = nullptr;
    this->m_videoBytes = 0;
    this->m_videoChunks = 0;
    this->m_maxFrameSize = 0;
    this->m_audioBytes = 0;
    this->m_audioChunks = 0;
    this->m_pad94 = 0;
}

// ref: FUN_14122dae0
CAviParser::~CAviParser() {
    this->Close();
}

// ref: FUN_14122db40
void CAviParser::Close() {
    SMemFree(this->m_paiv, s_aviParserFile, 0xc3, 0);
    this->m_paiv = nullptr;
    SMemFree(this->m_pviv, s_aviParserFile, 0xc4, 0);
    this->m_pviv = nullptr;
    if (this->m_file != nullptr) {
        cas::CASFileClose(&this->m_file);
        this->m_file = nullptr;
    }
    this->Reset();
}

// ref: FUN_14122dbc0
void CAviParser::Reset() {
    if (this->m_file != nullptr) {
        this->Close();
    }
    this->m_pviv = nullptr;
    this->m_paiv = nullptr;
    this->m_fourcc = 0;
    this->m_moviOffset = 0;
    this->m_fileDataId = 0;
    this->m_fps = 0.0f;
    this->m_width = 0;
    this->m_height = 0;
    this->m_frameCount = 0;
    this->m_videoKBps = 0;
    this->m_audioKBps = 0;
    this->m_totalKBps = 0;
    this->m_seconds = 0;
    this->m_videoBytes = 0;
    this->m_videoChunks = 0;
    this->m_maxFrameSize = 0;
    this->m_audioBytes = 0;
    this->m_audioChunks = 0;
}

// ref: FUN_14122dc10
void CAviParser::SetFile(int32_t fileDataId) {
    this->Reset();
    this->m_fileDataId = fileDataId;
}

// ref: FUN_14122dc70
bool CAviParser::Open() {
    if (this->m_fileDataId == 0 || !cas::CASFileOpenById(this->m_fileDataId, &this->m_file, 0) || !this->ReadHeaders()
        || this->m_index.size() == 0) {
        return false;
    }
    this->m_videoBytes = 0;
    this->m_videoChunks = 0;
    this->m_maxFrameSize = 0;
    uint32_t videoChunks = 0;
    uint32_t largest = 0;
    for (uint64_t i = 0; i < this->m_index.size(); i++) {
        const AVIINDEXENTRY& entry = this->m_index[i];
        char kind = static_cast<char>(entry.m_chunkId >> 16);
        if (kind == 'd') {
            videoChunks++;
            this->m_videoBytes += entry.m_size;
            if (largest < entry.m_size) {
                largest = entry.m_size;
            }
        } else if (kind == 'w') {
            this->m_audioChunks++;
            this->m_audioBytes += entry.m_size;
        }
    }
    this->m_maxFrameSize = largest;
    this->m_videoChunks = videoChunks;
    float seconds = static_cast<float>(videoChunks) / this->m_fps;
    float perSecond = 1.0f / seconds;
    this->m_seconds = static_cast<int32_t>(seconds);
    this->m_videoKBps = static_cast<int32_t>(static_cast<float>(this->m_videoBytes / 1000) * perSecond);
    this->m_audioKBps = static_cast<int32_t>(static_cast<float>(this->m_audioBytes / 1000) * perSecond);
    this->m_totalKBps = static_cast<int32_t>(static_cast<float>((this->m_audioBytes + this->m_videoBytes) / 1000) * perSecond);
    return this->BuildBlocks();
}

// The file's chunks: hdrl kept to read, the movi list's position, idx1 and the key chunks read
// in; then the hdrl's stream headers give the video's rate, length, size and compression.
// ref: FUN_14122ddf0
bool CAviParser::ReadHeaders() {
    uint64_t size = cas::CASFileGetSize(this->m_file);
    char header[12];
    uint32_t read = 0;
    if (!cas::CASFileReadEx(this->m_file, header, 0xc, &read, 0, -1) || read != 0xc || _strnicmp(header, "RIFF", 4) != 0
        || _strnicmp(header + 8, "AVI ", 4) != 0
        || static_cast<uint64_t>(cas::CASFileSetPosition(this->m_file, 0, 1)) >= size) {
        return false;
    }
    char* hdrl = nullptr;
    uint32_t hdrlSize = 0;
    for (;;) {
        char chunk[8];
        if (!cas::CASFileReadEx(this->m_file, chunk, 8, &read, 0, -1) || read != 8) {
            break;
        }
        uint32_t chunkSize;
        std::memcpy(&chunkSize, chunk + 4, 4);
        bool ok = true;
        if (_strnicmp(chunk, "LIST", 4) == 0) {
            if (!cas::CASFileReadEx(this->m_file, chunk, 4, &read, 0, -1) || read != 4) {
                break;
            }
            chunkSize -= 4;
            if (_strnicmp(chunk, "hdrl", 4) == 0) {
                // the reference's alloca
                hdrl = static_cast<char*>(_alloca(chunkSize));
                hdrlSize = chunkSize;
                if (!cas::CASFileReadEx(this->m_file, hdrl, chunkSize, &read, 0, -1) || read != chunkSize) {
                    break;
                }
            } else {
                if (_strnicmp(chunk, "movi", 4) == 0) {
                    this->m_moviOffset = static_cast<uint64_t>(cas::CASFileSetPosition(this->m_file, 0, 1));
                }
                cas::CASFileSetPosition(this->m_file, chunkSize, 1);
            }
        } else if (_strnicmp(chunk, "idx1", 4) == 0) {
            this->m_index.resize(chunkSize >> 4);
            ok = cas::CASFileReadEx(this->m_file, this->m_index.data(), chunkSize, &read, 0, -1) && read == chunkSize;
        } else if (_strnicmp(chunk, "pviv", 4) == 0) {
            this->m_pviv = SMemAlloc(chunkSize, 1, s_aviParserFile, SStrHashFnv1a(s_aviParserFile), 0x13a, 0);
            ok = cas::CASFileReadEx(this->m_file, this->m_pviv, chunkSize, &read, 0, -1) && read == chunkSize;
        } else if (_strnicmp(chunk, "paiv", 4) == 0) {
            this->m_paiv = SMemAlloc(chunkSize, 1, s_aviParserFile, SStrHashFnv1a(s_aviParserFile), 0x13f, 0);
            ok = cas::CASFileReadEx(this->m_file, this->m_paiv, chunkSize, &read, 0, -1) && read == chunkSize;
        } else {
            cas::CASFileSetPosition(this->m_file, chunkSize, 1);
        }
        if (!ok) {
            break;
        }
        if (static_cast<uint64_t>(cas::CASFileSetPosition(this->m_file, 0, 1)) >= size) {
            break;
        }
    }
    if (hdrl == nullptr) {
        return false;
    }
    int32_t stream = 0;
    int32_t videoStream = -1;
    int32_t audioStream = -1;
    uint32_t at = 0;
    while (at < hdrlSize) {
        const char* chunk = hdrl + at;
        if (_strnicmp(chunk, "LIST", 4) == 0) {
            at += 0xc;
            continue;
        }
        uint32_t chunkSize;
        std::memcpy(&chunkSize, chunk + 4, 4);
        at += 8;
        if (_strnicmp(chunk, "strh", 4) == 0) {
            const char* body = hdrl + at;
            if (_strnicmp(body, "vids", 4) == 0) {
                uint32_t scale;
                uint32_t rate;
                std::memcpy(&scale, body + 0x14, 4);
                std::memcpy(&rate, body + 0x18, 4);
                this->m_fps = static_cast<float>(rate) / static_cast<float>(scale);
                std::memcpy(&this->m_frameCount, body + 0x20, 4);
                videoStream = stream;
            }
            if (_strnicmp(body, "auds", 4) == 0) {
                audioStream = stream;
            }
            stream++;
        } else if (_strnicmp(chunk, "strf", 4) == 0 && videoStream >= 0 && audioStream < 0) {
            const char* body = hdrl + at;
            std::memcpy(&this->m_width, body + 4, 4);
            std::memcpy(&this->m_height, body + 8, 4);
            std::memcpy(&this->m_fourcc, body + 0x10, 4);
        }
        at += chunkSize;
    }
    return true;
}

// The file split into the blocks CAS streams it in: the headers and every chunk header as raw
// bytes, a LIST's type the same, the movi list's body as movie data, and whatever follows the
// RIFF as raw again. True when the blocks cover the whole file.
// ref: FUN_14122e300
bool CAviParser::BuildBlocks() {
    uint64_t fileSize = static_cast<uint32_t>(cas::CASFileGetSize(this->m_file));
    BLOCKLIST list;
    uint64_t at = 0;
    uint32_t riff[3] = {};
    if (0xb < fileSize) {
        cas::CASFileReadAtOffset(this->m_file, riff, 0, 0xc, nullptr, 0, -1);
        at = 0xc;
    }
    list.Push(0xc, 'z');
    uint64_t riffEnd = static_cast<uint64_t>(riff[1]) - 4 + at;
    for (;;) {
        uint64_t next = at + 8;
        if (riffEnd < next) {
            break;
        }
        uint32_t chunk[2] = {};
        cas::CASFileReadAtOffset(this->m_file, chunk, static_cast<uint32_t>(at), 8, nullptr, 0, -1);
        list.Append('z', 8);
        at = next;
        uint64_t padded = (static_cast<uint64_t>(chunk[1]) + 1) & ~1ull;
        uint64_t end = at + padded;
        if (chunk[0] != FOURCC('L', 'I', 'S', 'T')) {
            list.Append('z', padded);
            at = end;
            continue;
        }
        uint32_t type = 0;
        if (at + 4 <= end) {
            cas::CASFileReadAtOffset(this->m_file, &type, static_cast<uint32_t>(at), 4, nullptr, 0, -1);
        }
        list.Append('z', 4);
        list.Append(type == FOURCC('m', 'o', 'v', 'i') ? 'n' : 'z', padded - 4);
        at = end;
    }
    list.Append('z', fileSize - riffEnd);
    this->m_blocks.resize(list.m_blocks.size());
    uint64_t offset = 0;
    for (uint64_t i = 0; i < list.m_blocks.size(); i++) {
        this->m_blocks[i].m_offset = static_cast<uint32_t>(offset);
        this->m_blocks[i].m_size = static_cast<uint32_t>(list.m_blocks[i].m_size);
        offset += list.m_blocks[i].m_size;
    }
    return offset == fileSize;
}

// ---- IVideoDecoder ---------------------------------------------------------------------------

// ref: FUN_14122f5f0
IVideoDecoder::IVideoDecoder() {
}

// ref: FUN_14122f660
IVideoDecoder::~IVideoDecoder() {
}

// ref: FUN_14122fc30
bool IVideoDecoder::SetSize(int32_t width, int32_t height) {
    this->m_width = width;
    this->m_height = height;
    this->m_ready = this->Initialize() ? 1 : 0;
    return this->m_ready != 0;
}

// ref: FUN_14122fc50
void IVideoDecoder::Close() {
    this->m_width = 0;
    this->m_height = 0;
    this->Shutdown();
    this->m_ready = 0;
}

// ref: FUN_14122f790
bool IVideoDecoder::DecodeFrame(const void* data, uint32_t size) {
    return this->Decode(data, size);
}

// ref: FUN_14122f7a0
void IVideoDecoder::Render(const float* rect) {
    if (!(rect[0] < rect[2]) || !(rect[1] < rect[3]) || this->m_width == 0 || this->m_height == 0) {
        return;
    }
    GxRenderContext* context = g_gxRenderContext;
    float saved[6];
    for (uint32_t i = 0; i < 6; i++) {
        saved[i] = context->Field<float>(0x1c + i * 4);
    }
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    // the picture kept to its own shape inside the rectangle {minY, minX, maxY, maxX}
    float insetY = 0.0f;
    float insetX = 0.0f;
    float rectAspect = (rect[3] - rect[1]) / (rect[2] - rect[0]);
    float pictureAspect = static_cast<float>(this->m_width) / static_cast<float>(this->m_height);
    if (rectAspect < pictureAspect) {
        insetY = ((pictureAspect - rectAspect) / pictureAspect) * 0.5f;
    } else if (pictureAspect < rectAspect) {
        insetX = ((rectAspect - pictureAspect) / rectAspect) * 0.5f;
    }
    float maxX = rect[3] / g_coordinateSpace.m_diagonalWidth;
    float maxY = rect[2] / g_coordinateSpace.m_diagonalHeight;
    float minX = rect[1] / g_coordinateSpace.m_diagonalWidth;
    float minY = rect[0] / g_coordinateSpace.m_diagonalHeight;
    insetY = (maxY - minY) * insetY;
    insetX = (maxX - minX) * insetX;
    minY += insetY;
    maxY -= insetY;
    minX += insetX;
    maxX -= insetX;
    this->Bind();
    float textureSize[2];
    this->GetMaxSize(textureSize);
    float v = static_cast<float>(this->m_height) / textureSize[1];
    float u = static_cast<float>(this->m_width) / textureSize[0];
    uint32_t stride = GxVertexFormatStride(4);
    GxBuf* vertices = GxBufTransient(context, 0, stride, 4);
    auto data = static_cast<uint8_t*>(context->BufLock(vertices));
    uint8_t* position = data + GxVertexFormatOffset(4, 0);
    uint8_t* normal = data + GxVertexFormatOffset(4, 3);
    uint8_t* uv = data + GxVertexFormatOffset(4, 6);
    // DAT_142cfb9b0: the quad faces +z
    const float facing[3] = { 0.0f, 0.0f, 1.0f };
    const float corners[4][4] = {
        { minX, minY, 0.0f, v },
        { maxX, minY, u, v },
        { minX, maxY, 0.0f, 0.0f },
        { maxX, maxY, u, 0.0f },
    };
    for (uint32_t i = 0; i < 4; i++) {
        auto p = reinterpret_cast<float*>(position + i * stride);
        p[0] = corners[i][0];
        p[1] = corners[i][1];
        p[2] = 0.0f;
        std::memcpy(normal + i * stride, facing, sizeof(facing));
        auto t = reinterpret_cast<float*>(uv + i * stride);
        t[0] = corners[i][2];
        t[1] = corners[i][3];
    }
    context->BufUnlock(vertices, stride * 4);
    GxSetVertexStream(context, vertices, 4);
    // DAT_1427233d8
    static const uint16_t s_indices[4] = { 0, 1, 2, 3 };
    GxBuf* indices = GxBufTransient(context, 1, 2, 4);
    context->BufWrite(indices, s_indices, 8, 0);
    context->SetIndexBuffer(indices, 1);
    context->DrawIndexed(4, 1, 0, 0, 0, 0);
    GxXformSetViewport(saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]);
}

// ---- CVideoDecoderVPX ------------------------------------------------------------------------

namespace {

const char s_decoderListTag[] = ".?AV?$TSGrowableArray@PEAVCVideoDecoderVPX@movieplayerv1@@$07@@";

// A plane's CPU copy as the texture upload reads it (the reference's reader, LAB_1405e13c0).
struct PLANEREAD {
    uint32_t m_pitch;
    uint32_t m_pad;
    const void* m_data;
};

void ReadPlane(void* param, uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)width;
    (void)height;
    (void)slice;
    (void)mip;
    auto read = static_cast<PLANEREAD*>(param);
    *pitch = read->m_pitch;
    *data = read->m_data;
}

// The packets' stream cipher (a 32-byte key and IV, FUN_1420bd880..). Only movies with a key
// chunk ('pviv') are encrypted, and none of 3.4.3's are.
struct MOVIECIPHER {
    uint8_t m_key[0x20];
    uint8_t m_iv[0x20];

    void Process(uint8_t* data, uint32_t size) {
        (void)data;
        (void)size;
        FROST_UNIMPLEMENTED();  // FUN_1420bd960: the key stream XORed in place
    }
};

class CVideoDecoderVPX;

// Every VPX decoder with device objects (DAT_1430a13f0), and the last picture decoded
// (DAT_1430a1410), drawn again when no new one has come.
blz::vector<CVideoDecoderVPX*, s_decoderListTag> s_decoders;
const vpx_image_t* s_lastImage;

void OnDeviceCreated(void* param);
void OnDeviceDestroyed(void* param);

class CVideoDecoderVPX : public IVideoDecoder {
    public:
    // ref: FUN_14122fc70
    CVideoDecoderVPX() {
        std::memset(&this->m_codec, 0, sizeof(this->m_codec));
        for (uint32_t i = 0; i < 3; i++) {
            std::memset(this->m_texDesc[i], 0, sizeof(this->m_texDesc[i]));
            this->m_textures[i] = nullptr;
        }
    }

    // ref: FUN_14122ff30
    ~CVideoDecoderVPX() override {
        this->Close();
    }

    // ref: FUN_141230080
    void GetMaxSize(float* size) override {
        // DAT_142723668
        size[0] = 2048.0f;
        size[1] = 2048.0f;
    }

    // ref: FUN_141230a70
    bool Initialize() override {
        if (!this->InitCodec()) {
            return false;
        }
        s_decoders.push_back(this);
        if (!this->CreateDeviceObjects()) {
            return false;
        }
        this->m_buffer.resize(static_cast<uint64_t>(this->m_height) * this->m_width + 4);
        if (s_decoders.size() == 1) {
            GxAddDeviceCreatedCallback(&OnDeviceCreated, nullptr);
            GxAddDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
        }
        return true;
    }

    // The frame behind four zero bytes in the decode buffer, through the key stream when the
    // movie is encrypted, then to libvpx.
    // ref: FUN_141230390
    bool Decode(const void* data, uint32_t size) override {
        uint8_t* buffer = this->Stage(data, size);
        return vpx_codec_decode(&this->m_codec, buffer + 4, size, nullptr, 0) == VPX_CODEC_OK;
    }

    // ref: FUN_141230440
    bool Skip(const void* data, uint32_t size) override {
        this->Stage(data, size);
        return true;
    }

    // ref: FUN_141230b20
    void Shutdown() override {
        for (int64_t i = static_cast<int64_t>(s_decoders.size()) - 1; i >= 0; i--) {
            if (s_decoders[i] == this) {
                s_decoders[i] = s_decoders[s_decoders.size() - 1];
                s_decoders.pop_back();
                break;
            }
        }
        if (s_decoders.size() == 0) {
            GxRemoveDeviceCreatedCallback(&OnDeviceCreated, nullptr);
            GxRemoveDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
        }
        this->DestroyDeviceObjects();
        this->m_buffer.resize(0);
        if (this->m_codecReady != 0) {
            vpx_codec_destroy(&this->m_codec);
            this->m_codecReady = 0;
        }
    }

    // The newest picture's planes copied into the textures' CPU images and uploaded; the
    // pipeline and the three textures bound for the pixel shader.
    // ref: FUN_141230790
    void Bind() override {
        vpx_codec_iter_t iterator = nullptr;
        const vpx_image_t* image = vpx_codec_get_frame(&this->m_codec, &iterator);
        const vpx_image_t* last = s_lastImage;
        while ((s_lastImage = image) != nullptr) {
            image = vpx_codec_get_frame(&this->m_codec, &iterator);
            last = s_lastImage;
        }
        s_lastImage = last;
        GxRenderContext* context = g_gxRenderContext;
        if (last != nullptr) {
            const int32_t rows[3] = { this->m_height, this->m_height / 2, this->m_height / 2 };
            const int32_t bytes[3] = { this->m_width, this->m_width / 2, this->m_width / 2 };
            for (uint32_t plane = 0; plane < 3; plane++) {
                int32_t pitch = this->TexWidth(plane);
                uint8_t* dest = this->m_planes[plane].data();
                for (int32_t row = 0; row < rows[plane]; row++) {
                    std::memcpy(dest + static_cast<uint32_t>(row * pitch),
                        last->planes[plane] + static_cast<uint32_t>(row * last->stride[plane]), bytes[plane]);
                }
            }
        }
        for (uint32_t plane = 0; plane < 3; plane++) {
            int32_t rect[4] = { 0, 0, this->TexHeight(plane), this->TexWidth(plane) };
            PLANEREAD read = { plane == 0 ? 0x800u : 0x400u, 0, this->m_planes[plane].data() };
            GxTexUpdateInFrame(context, this->m_textures[plane], rect, false, &ReadPlane, &read);
        }
        context->SetPipeline(this->m_pipeline);
        for (uint32_t plane = 0; plane < 3; plane++) {
            context->SetTexture(2, plane, static_cast<gxDx12Backend::Tex*>(this->m_textures[plane]));
        }
    }

    // ref: FUN_141230d00
    void SetKey(const void* key, const void* iv) override {
        this->m_encrypted = 1;
        std::memcpy(this->m_cipher.m_key, key, 0x20);
        std::memcpy(this->m_cipher.m_iv, iv, 0x20);
    }

    // ref: FUN_141230030
    void GetError(char* buffer, uint32_t size) override {
        std::snprintf(buffer, size, "%s : %s", vpx_codec_err_to_string(this->m_codec.err),
            this->m_codec.err_detail != nullptr ? this->m_codec.err_detail : "");
    }

    // The YUV pipeline ("CVideoDecoderVPX", shaders 0x4e and 0x88) and the three plane
    // textures, 2048 square for Y and 1024 for U and V, their CPU images black and grey.
    // ref: FUN_141230140
    bool CreateDeviceObjects() override {
        if (this->m_codecReady == 0 || this->m_deviceReady != 0) {
            return false;
        }
        GXPIPELINEKEY key;
        std::memset(&key, 0, sizeof(key));
        key.m_topology = 4;
        uint32_t raster = 0x10000;
        std::memcpy(key.m_raster, &raster, 4);
        uint64_t depthStencil = 0x700000020ull;
        std::memcpy(key.m_depthStencil, &depthStencil, 8);
        RENDERTARGETFORMATS formats = RenderTargetFormats(0xc);
        std::memcpy(key.m_targetFormats, &formats, 4);
        key.m_depthFormat = formats.m_depth;
        uint32_t one = 1;
        std::memcpy(reinterpret_cast<uint8_t*>(&key) + 0x20, &one, 4);
        key.m_vertexShader = 0x4e;
        key.m_pixelShader = 0x88;
        key.m_vertexFormat = 4;
        key.m_blend = 0xf000;
        this->m_pipeline = GxPipelineCreate(&key, "CVideoDecoderVPX");
        const int32_t sizes[3] = { 0x800, 0x400, 0x400 };
        const uint8_t fills[3] = { 0x00, 0x7f, 0x7f };
        for (uint32_t i = 0; i < 3; i++) {
            uint8_t* desc = this->m_texDesc[i];
            std::memset(desc, 0, sizeof(this->m_texDesc[i]));
            std::memcpy(desc + 4, &sizes[i], 4);
            std::memcpy(desc + 8, &sizes[i], 4);
            uint32_t depth = 1;
            std::memcpy(desc + 0xc, &depth, 4);
            uint16_t format = 0xc0c;
            std::memcpy(desc + 0x10, &format, 2);
            uint64_t flags = 0x82001;
            std::memcpy(desc + 0x14, &flags, 8);
            this->m_planes[i].resize(static_cast<uint64_t>(sizes[i]) * sizes[i]);
            std::memset(this->m_planes[i].data(), fills[i], this->m_planes[i].size());
            if (!GxTexCreate(desc, &this->m_textures[i])) {
                GxPipelineDestroy(&this->m_pipeline);
                return false;
            }
        }
        this->m_deviceReady = 1;
        return true;
    }

    // ref: FUN_1412304c0
    bool DestroyDeviceObjects() override {
        if (this->m_deviceReady == 0) {
            return false;
        }
        this->m_deviceReady = 0;
        GxPipelineDestroy(&this->m_pipeline);
        for (uint32_t i = 0; i < 3; i++) {
            GxTexDestroy(&this->m_textures[i]);
            this->m_planes[i].resize(0);
            std::memset(this->m_texDesc[i], 0, sizeof(this->m_texDesc[i]));
        }
        return true;
    }

    // libvpx's VP9 decoder (the reference reaches it through function pointers it fills here).
    // ref: FUN_1412300a0
    bool InitCodec() override {
        if (vpx_codec_dec_init(&this->m_codec, vpx_codec_vp9_dx(), nullptr, 0) != VPX_CODEC_OK) {
            this->Close();
            return false;
        }
        this->m_codecReady = 1;
        return true;
    }

    private:
    uint8_t* Stage(const void* data, uint32_t size) {
        uint8_t* buffer = this->m_buffer.data();
        buffer[0] = 0;
        buffer[1] = 0;
        buffer[2] = 0;
        buffer[3] = 0;
        std::memcpy(buffer + 4, data, size);
        if (this->m_encrypted != 0) {
            this->m_cipher.Process(buffer, size + 4);
        }
        return buffer;
    }

    int32_t TexWidth(uint32_t plane) {
        int32_t width;
        std::memcpy(&width, this->m_texDesc[plane] + 4, 4);
        return width;
    }

    int32_t TexHeight(uint32_t plane) {
        int32_t height;
        std::memcpy(&height, this->m_texDesc[plane] + 8, 4);
        return height;
    }

    vpx_codec_ctx_t m_codec;                                    // 0x58
    uint8_t m_codecReady = 0;                                   // 0x90
    uint8_t m_deviceReady = 0;                                  // 0x91
    GxPipeline* m_pipeline = nullptr;                           // 0x98
    uint8_t m_encrypted = 0;                                    // 0xA0
    MOVIECIPHER m_cipher;                                       // 0xB0
    blz::vector<uint8_t, s_byteTag> m_buffer;                   // 0x580
    uint8_t m_texDesc[3][0x38];                                 // 0x598
    void* m_textures[3];                                        // 0x640
    blz::vector<uint8_t, s_byteTag> m_planes[3];                // 0x658

    friend void OnDeviceCreated(void* param);
    friend void OnDeviceDestroyed(void* param);
};

// ref: FUN_141230710
void OnDeviceCreated(void* param) {
    (void)param;
    for (uint64_t i = 0; i < s_decoders.size(); i++) {
        s_decoders[i]->CreateDeviceObjects();
    }
}

// ref: FUN_141230750
void OnDeviceDestroyed(void* param) {
    (void)param;
    for (uint64_t i = 0; i < s_decoders.size(); i++) {
        s_decoders[i]->DestroyDeviceObjects();
    }
}

}  // namespace

// ref: FUN_14122f690
IVideoDecoder* VideoDecoderCreate(uint32_t fourcc) {
    if (fourcc == FOURCC('V', 'P', '9', '0') || fourcc == FOURCC('V', 'P', '9', 'P')) {
        void* memory = SMemAlloc(sizeof(CVideoDecoderVPX), 1, s_decoderFile, SStrHashFnv1a(s_decoderFile), 0x9e, 0);
        return new (memory) CVideoDecoderVPX();
    }
    if (fourcc == FOURCC('A', 'V', '0', '1')) {
        FROST_UNIMPLEMENTED();  // CVideoDecoderDav1d (0x2b0, FUN_141230db0): no 3.4.3 movie is AV1
        return nullptr;
    }
    return nullptr;
}

}  // namespace movieplayerv1
