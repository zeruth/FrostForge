#ifndef AIO_AIO_HPP
#define AIO_AIO_HPP

// pkg/aio/1.1.12: asynchronous file I/O over an I/O completion port with one "AIO Thread"
// draining it. tact owns one context per instance; casc submits its reads through it.

#include "blz/Allocator.hpp"
#include "blz/IdMap.hpp"
#include "os/Os.hpp"
#include <cstdint>
#include <windows.h>

namespace aio {

struct CONTEXT;

typedef void (*CALLBACK_)(void* context, CONTEXT* aio, uint64_t key);

// One read or write in flight (0x78 bytes). The OVERLAPPED comes first so a completion packet
// leads back to it.
struct REQUEST {
    OVERLAPPED m_overlapped;            // 0x00
    HANDLE m_file;                      // 0x20
    HANDLE m_event;                     // 0x28
    uint32_t m_status;                  // 0x30: Win32 error, ERROR_IO_PENDING while queued
    uint32_t m_transferred;             // 0x34
    uint8_t m_38;                       // 0x38
    uint64_t m_key;                     // 0x40
    CALLBACK_ m_callback;               // 0x48
    void* m_context;                    // 0x50
    uint32_t m_size;                    // 0x58: what the caller asked for
    uint64_t m_offset;                  // 0x60
    void* m_destination;                // 0x68: where an unaligned read is copied to
    void* m_bounce;                     // 0x70: the sector-aligned buffer actually read into
};
static_assert(sizeof(REQUEST) == 0x78, "aio::REQUEST is 0x78 bytes");

// The context (0x70 bytes).
struct CONTEXT {
    blz::allocator* m_allocator;        // 0x00
    uint8_t m_buffered;                 // 0x08: files are opened buffered (no alignment needed)
    HANDLE m_port;                      // 0x10
    OSTHREAD m_thread;                  // 0x18
    volatile uint8_t m_stop;            // 0x30
    SRWLOCK m_lock;                     // 0x38
    blz::id_map<REQUEST*> m_requests;   // 0x40: in flight, by key
};
static_assert(sizeof(CONTEXT) == 0x70, "aio::CONTEXT is 0x70 bytes");

// How a context is created; the IO thread gets the affinity, priority and stack size.
struct PARAMS {
    uint64_t m_affinity;                // 0x00
    int32_t m_priority;                 // 0x08
    uint32_t m_0c;                      // 0x0c
    uint32_t m_stackSize;               // 0x10
    uint64_t m_18;                      // 0x18
    uint64_t m_20;                      // 0x20
    uint8_t m_buffered;                 // 0x28
    blz::allocator* m_allocator;        // 0x30: null for the process allocator
};

enum RESULT : uint8_t {
    RESULT_SUCCESS = 0,
    RESULT_INVALID_PARAM = 1,
    RESULT_NOT_SUPPORTED = 2,
    RESULT_ABORTED = 3,
    RESULT_NOT_FOUND = 4,
    RESULT_ACCESS_DENIED = 5,
    RESULT_END_OF_FILE = 6,
    RESULT_FAILED = 7,
    RESULT_SHARING_VIOLATION = 8,
};

CONTEXT* ContextCreate(PARAMS* params, uint8_t* result);
void ContextDestroy(CONTEXT* context);
uint8_t ResultFromError(uint32_t error);
const char* ResultString(uint8_t result);
uint64_t ResultStringCopy(char* out, uint64_t size, uint8_t result);
void LogError(uint32_t error, const char* message);

// Opens a file with the packages' modes (0 write, 1 read/write, 2 read existing, 3 read/write
// existing, 4 write new, 5 read/write new) and binds it to the port.
uint8_t FileOpen(CONTEXT* context, OSFILE* file, const char* path, uint8_t mode);
// Starts a read; returns its key (0 when it could not be started).
uint64_t FileRead(CONTEXT* context, OSFILE* file, void* buffer, uint64_t offset, uint32_t size, CALLBACK_ callback,
    void* callbackContext);
// Waits for a request and returns its result, with the bytes transferred; forgets the request.
uint8_t FileResult(CONTEXT* context, uint64_t key, uint32_t* transferred);
uint8_t FileCancel(CONTEXT* context, uint64_t key);
bool FileIsDone(CONTEXT* context, uint64_t key);

}

#endif
