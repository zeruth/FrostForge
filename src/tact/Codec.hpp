#ifndef TACT_CODEC_HPP
#define TACT_CODEC_HPP

// pkg/tact/3.6.39/src/codec: decoding encoded (BLTE) file data. An encoded file is "BLTE", a
// big-endian header size, then either one block or a header listing the blocks (each with its
// sizes and the MD5 of its encoded bytes); every block starts with its codec: 'N' none,
// 'Z' zlib, '4' lz4hc (compiled out), 'E' encrypted (Salsa20 around another block).
// Unencoded data is decoded by its e-spec instead ("n", "z", "c:{...}" BCPack, "b:..." blocks).

#include <cstdint>

namespace tact {

struct SCRATCHBUFFER;
struct U32SET;

// The client's key lookup for encrypted blocks: (context, 8-byte key name, 16-byte key out).
typedef bool (*KEYGETTER)(void* context, const uint8_t* name, uint8_t* key);

// A decoder the client may put in front of zlib: false from Decode means it handled the data.
class DECODERHOOK {
    public:
    virtual ~DECODERHOOK() = default;
    virtual bool Decode(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t* codecByte) = 0;
};

// The parsed encoding header (0x20 bytes) and its block entries (0x18 bytes each).
struct ENCODINGBLOCK {
    uint32_t m_decodedSize;         // 0x00
    uint32_t m_encodedSize;         // 0x04
    uint8_t m_hash[16];             // 0x08: MD5 of the encoded block
};
struct ENCODINGHEADER {
    uint8_t m_hash[16];             // 0x00: MD5 of the header
    ENCODINGBLOCK* m_blocks;        // 0x10: after an 8-byte count
    uint32_t m_blockCount;          // 0x18
};

// e-spec codec types (FUN_1420d8300).
enum {
    ESPEC_INVALID = 0,
    ESPEC_NONE = 1,
    ESPEC_ZLIB = 2,
    ESPEC_LZ4HC = 3,
    ESPEC_BCPACK = 4,
    ESPEC_ENCRYPTED = 5,
    ESPEC_BLOCKS = 6,
    ESPEC_G = 7,
};

struct ZLIBSPEC {
    int32_t m_level;                // 0x00: 9
    int32_t m_windowBits;           // 0x04: 15
    uint8_t m_mpq;                  // 0x08
    int32_t m_variant;              // 0x0c: 0 zlib, 1 lz4hc
};

uint8_t ESpecType(const char* spec, const char* end);
bool ESpecNumber(const char** cursor, const char* end, bool* present, int32_t* value);
uint8_t ESpecParseZlib(const char* cursor, const char* end, bool braced, ZLIBSPEC* spec);
uint8_t ESpecParseBcn(const char* spec, const char* end, int32_t* bcn);

// A block of a "b:{...}" e-spec (0x18): the decoded size of each block, how many, and the
// sub-spec the blocks are encoded with.
struct ESPECBLOCK {
    uint32_t m_size;
    uint32_t m_count;
    const char* m_spec;
    const char* m_specEnd;
};

// FUN_1420d77a0: one "<size>[K|M][*<count>]=" token.
bool ESpecBlockToken(const char** cursor, const char* end, int32_t* size, int32_t* count, char* variable);
// FUN_1420d7e30: a block spec's blocks for a file of `csize` decoded bytes (an array from the
// buffer allocator, freed with ESpecFreeBlocks).
uint8_t ESpecParseBlocks(uint32_t csize, const char* spec, const char* end, ESPECBLOCK** blocks, int32_t* count);
// FUN_1420d7bf0
void ESpecFreeBlocks(ESPECBLOCK* blocks);
// FUN_1420f9950: the encoding header size a block spec implies.
uint8_t ESpecEncodingHeaderSize(const char* spec, uint32_t csize, uint32_t* headerSize);
// FUN_1420d79e0: a hex argument at *cursor, up to `size` bytes.
bool ESpecHexArgument(const char** cursor, const char* end, uint8_t* out, uint32_t size, uint32_t* decoded);
// FUN_1420d84d0: "e:{key name,IV,nested}".
uint8_t ESpecParseEncryption(const char* spec, const char* end, uint8_t* keyName, uint8_t* iv, uint32_t* ivSize,
    const char** nested, const char** nestedEnd);
// FUN_1420d8a00: the encryption key names over a byte range; the array is freed with
// ESpecFreeBlocks.
uint8_t ESpecGetKeyNames(const char* spec, uint32_t csize, uint64_t** keys, uint32_t* count, uint32_t offset,
    uint32_t size);

// Whether data starts with an encoding header ("BLTE"); *error on a malformed one.
bool CodecIsEncoded(const uint8_t* data, uint32_t size, uint8_t* error);

uint8_t EncodingHeaderParse(const uint8_t* ekey, uint64_t ekeySize, const uint8_t* data, uint32_t size,
    ENCODINGHEADER** header, uint32_t* headerSize);
ENCODINGHEADER* EncodingHeaderCreate(const uint8_t* ekey, uint64_t ekeySize, const uint8_t* data, uint32_t size,
    uint32_t* headerSize, uint8_t* error);
void EncodingHeaderFree(ENCODINGHEADER* header);
uint32_t EncodingHeaderSize(uint32_t blockCount);
// The whole blocks covering a decoded span: their encoded span (past the header) and decoded span.
uint8_t EncodingHeaderMapSpan(const ENCODINGHEADER* header, const uint32_t cSpan[2], uint32_t eSpan[2],
    uint32_t* aligned);
// The blocks an encoded span covers, and their decoded span.
uint8_t EncodingHeaderBlockRange(const ENCODINGHEADER* header, const uint32_t eSpan[2], uint32_t* first,
    uint32_t* end, uint32_t cSpan[2]);

bool CodecVerifyHash(const uint8_t* hash, uint64_t hashSize, const void* data, uint32_t size);

// Decodes one block (with its codec byte) into out.
uint8_t CodecDecodeEncoded(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint32_t blockIndex,
    const uint8_t* blockHash, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook);
uint8_t DecodeBlocks(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, ENCODINGHEADER* header,
    uint32_t firstBlock, const ENCODINGBLOCK* begin, const ENCODINGBLOCK* end, U32SET* corrupt, uint8_t a10,
    KEYGETTER keyGetter, void* keyContext, uint8_t verify, SCRATCHBUFFER* scratch, DECODERHOOK* hook);

// Decodes a whole file by its e-spec (an empty spec: the data carries an encoding header).
uint8_t CodecDecode(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, const uint8_t* ekey,
    const char* eSpec, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook, U32SET* corrupt);
uint8_t CodecDecodeRaw(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, const uint8_t* ekey,
    const char* eSpec, uint8_t hashSize, uint8_t a8, KEYGETTER keyGetter, void* keyContext, uint8_t verify);
// Decodes the blocks an encoded span covers and keeps the decoded span asked for.
uint8_t CodecDecodeSpan(void* out, const uint32_t cSpan[2], const uint8_t* data, const uint32_t eSpan[2],
    const ENCODINGHEADER* header, U32SET* corrupt, uint8_t a7, KEYGETTER keyGetter, void* keyContext, uint8_t verify,
    SCRATCHBUFFER* scratch, DECODERHOOK* hook);

uint8_t CodecUncompressed(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte);
uint8_t CodecZlib(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte, uint8_t streamed,
    DECODERHOOK* hook);
uint8_t CodecLz4hc(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, uint8_t codecByte, uint8_t a6,
    SCRATCHBUFFER* scratch);
uint8_t CodecBcPack(void* out, uint32_t outSize, const uint8_t* data, uint32_t size, int32_t bcn, DECODERHOOK* hook);

uint8_t EncryptionHeaderSize(const uint8_t* data, uint32_t size, uint32_t* headerSize);
uint8_t CodecDecrypt(uint8_t* data, uint32_t size, uint32_t payloadSize, uint64_t blockIndex, KEYGETTER keyGetter,
    void* keyContext);

}

#endif
