#include "aio/Aio.hpp"
#include "blz/Log.hpp"
#include <cstring>
#include <malloc.h>

namespace aio {

namespace {

const char s_category[] = "aio";  // DAT_1428f5754
const char s_win32File[] = "D:\\BuildServer\\A\\work\\package_cache\\aio\\1.1.12\\src\\aio.win32.cpp";
const char s_internalFile[] = "D:\\BuildServer\\A\\work\\package_cache\\aio\\1.1.12\\src\\internal.cpp";

const char* const s_resultStrings[] = {
    "success",
    "invalid param",
    "operation not supported",
    "operation cancelled",
    "not found",
    "access denied",
    "truncated read",
    "unknown failure",
    "sharing violation",
};

// DAT_1428f5590: what FileRead returns when no event can be created.
const uint64_t s_noKey = 0;

volatile LONG64 s_lastKey = 0;  // DAT_143477868

enum : ULONG_PTR {
    KEY_REQUEST = 1,
    KEY_STOP = 2,
};

// ref: FUN_1421691f0
uint64_t NextKey() {
    return static_cast<uint64_t>(InterlockedIncrement64(&s_lastKey));
}

// The sector-aligned length covering [offset, offset + size) from the aligned offset.
// ref: FUN_142169150
uint32_t AlignedSize(uint32_t size, uint32_t offset, uint32_t alignedOffset) {
    return (size + 0xFFF + (offset - alignedOffset)) & 0xFFFFF000u;
}

// The IO thread: dequeues up to four completions at a time with a 2 ms timeout; a request's
// unaligned result is copied out of its bounce buffer, the buffer freed, its event set and its
// callback called. Runs until the stop flag is set (Destroy posts a stop packet to wake it).
// ref: FUN_142168230
void IoThread(void* argument) {
    auto context = static_cast<CONTEXT*>(argument);
    OsThreadSetName("AIO Thread");
    do {
        OVERLAPPED_ENTRY entries[4];
        ULONG count = 0;
        if (!GetQueuedCompletionStatusEx(context->m_port, entries, 4, &count, 2, FALSE)) {
            if (GetLastError() != WAIT_TIMEOUT) {
                LogError(GetLastError(), "AIO Thread failed to dequeue completion status.");
                return;
            }
            continue;
        }
        for (uint32_t i = 0; i < count; i++) {
            ULONG_PTR key = entries[i].lpCompletionKey;
            if (key == 0) {
                continue;
            }
            if (key == KEY_REQUEST) {
                auto request = reinterpret_cast<REQUEST*>(entries[i].lpOverlapped);
                if (request->m_status == ERROR_IO_PENDING) {
                    uint32_t status = 0;
                    if (!GetOverlappedResult(request->m_file, &request->m_overlapped,
                            reinterpret_cast<DWORD*>(&request->m_transferred), FALSE)) {
                        status = GetLastError();
                    }
                    request->m_status = status;
                }
                if (request->m_destination && request->m_bounce && request->m_transferred != 0) {
                    uint32_t size = request->m_size;
                    if (request->m_transferred < size) {
                        size = request->m_transferred;
                    }
                    std::memcpy(request->m_destination,
                        static_cast<uint8_t*>(request->m_bounce) + (request->m_offset - (request->m_offset & ~uint64_t(0xFFF))),
                        size);
                }
                context->m_allocator->deallocate(request->m_bounce);
                CALLBACK_ callback = request->m_callback;
                void* callbackContext = request->m_context;
                uint64_t requestKey = request->m_key;
                request->m_bounce = nullptr;
                SetEvent(request->m_event);
                if (callback) {
                    callback(callbackContext, context, requestKey);
                }
            } else if (key != KEY_STOP) {
                blz::log(s_category, 3, s_win32File, 0x28e, "Encountered unknown completion key (%u), skipping.",
                    static_cast<uint32_t>(key));
            }
        }
    } while (context->m_stop == 0);
}

// Cancels what is in flight, stops and joins the IO thread, and frees the context.
void Teardown(CONTEXT* context) {
    blz::allocator* allocator = context->m_allocator;
    AcquireSRWLockExclusive(&context->m_lock);
    auto entry = context->m_requests.m_entries;
    auto end = entry + context->m_requests.m_count;
    for (; entry != end; entry++) {
        CancelIoEx(entry->m_value->m_file, &entry->m_value->m_overlapped);
    }
    ReleaseSRWLockExclusive(&context->m_lock);
    if (context->m_port != INVALID_HANDLE_VALUE) {
        InterlockedExchange8(reinterpret_cast<volatile char*>(&context->m_stop), 1);
        PostQueuedCompletionStatus(context->m_port, 0, KEY_STOP, nullptr);
        OsThreadJoin(&context->m_thread);
        CloseHandle(context->m_port);
    }
    context->m_requests.destroy();
    OsThreadWaitStarted(&context->m_thread);
    allocator->deallocate(context);
}

}

// ref: FUN_142169170
void LogError(uint32_t error, const char* message) {
    char text[1024];
    OsErrorString(text, sizeof(text), error);
    blz::log(s_category, 4, s_internalFile, 0x50, "%s Error (%u): %s", message, error, text);
}

// ref: FUN_142168430
uint8_t ResultFromError(uint32_t error) {
    if (error < 6) {
        if (error == ERROR_ACCESS_DENIED) {
            return RESULT_ACCESS_DENIED;
        }
        if (error == ERROR_SUCCESS) {
            return RESULT_SUCCESS;
        }
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return RESULT_NOT_FOUND;
        }
    } else {
        if (error == ERROR_HANDLE_EOF) {
            return RESULT_END_OF_FILE;
        }
        if (error == ERROR_OPERATION_ABORTED) {
            return RESULT_ABORTED;
        }
    }
    LogError(error, "Encountered an unexpected result.");
    return RESULT_FAILED;
}

// ref: FUN_142169210
const char* ResultString(uint8_t result) {
    return s_resultStrings[result];
}

// ref: FUN_142169220
uint64_t ResultStringCopy(char* out, uint64_t size, uint8_t result) {
    const char* text = s_resultStrings[result];
    uint64_t length = std::strlen(text);
    if (size != 0) {
        uint64_t copy = length;
        if (size - 1 < length) {
            copy = size - 1;
        }
        std::memcpy(out, text, copy);
        out[copy] = '\0';
    }
    return length;
}

// ref: FUN_142168ea0
CONTEXT* ContextCreate(PARAMS* params, uint8_t* result) {
    blz::allocator* allocator = params->m_allocator;
    if (!allocator) {
        allocator = blz::default_allocator();
    }
    auto context = static_cast<CONTEXT*>(allocator->allocate(sizeof(CONTEXT), 8));
    CONTEXT* created = nullptr;
    if (context) {
        context->m_allocator = allocator;
        context->m_buffered = 1;
        context->m_port = INVALID_HANDLE_VALUE;
        context->m_thread.m_handle = nullptr;
        context->m_thread.m_function = nullptr;
        context->m_thread.m_argument = nullptr;
        context->m_stop = 0;
        context->m_requests.init(allocator);
        InitializeSRWLock(&context->m_lock);
    }
    context->m_buffered = params->m_buffered;

    // A guard (FUN_142168120) tears the half-built context down on every failure path.
    context->m_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (context->m_port == INVALID_HANDLE_VALUE) {
        LogError(GetLastError(), "Failed to create IO Completion Port.");
        if (result) {
            *result = ResultFromError(GetLastError());
        }
    } else {
        uint32_t error = OsThreadCreate(&context->m_thread, &IoThread, context, params->m_stackSize);
        if (error == 0) {
            OsThreadSetAffinity(&context->m_thread, params->m_affinity);
            OsThreadSetPriority(&context->m_thread, params->m_priority);
            created = context;
            if (result) {
                *result = RESULT_SUCCESS;
            }
        } else {
            LogError(error, "Failed to create IO thread.");
            if (result) {
                *result = RESULT_FAILED;
            }
            CloseHandle(context->m_port);
            context->m_port = INVALID_HANDLE_VALUE;
        }
    }
    if (!created && context) {
        Teardown(context);
    }
    return created;
}

// ref: FUN_142169050
void ContextDestroy(CONTEXT* context) {
    if (context) {
        Teardown(context);
    }
}

// ref: FUN_142168830
uint8_t FileOpen(CONTEXT* context, OSFILE* file, const char* path, uint8_t mode) {
    uint32_t access;
    uint32_t disposition;
    switch (mode) {
        case 0:
            access = GENERIC_WRITE;
            disposition = OPEN_ALWAYS;
            break;
        case 1:
            access = GENERIC_READ | GENERIC_WRITE;
            disposition = OPEN_ALWAYS;
            break;
        case 2:
            access = GENERIC_READ;
            disposition = OPEN_EXISTING;
            break;
        case 3:
            access = GENERIC_READ | GENERIC_WRITE;
            disposition = OPEN_EXISTING;
            break;
        case 4:
            access = GENERIC_WRITE;
            disposition = CREATE_ALWAYS;
            break;
        case 5:
            access = GENERIC_READ | GENERIC_WRITE;
            disposition = CREATE_ALWAYS;
            break;
        default:
            return RESULT_INVALID_PARAM;
    }
    uint8_t buffered = context->m_buffered;
    uint64_t length = std::strlen(path);
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    uint32_t flags = buffered ? FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL
                              : FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
        flags, nullptr);
    file->m_handle = handle;
    if (handle == INVALID_HANDLE_VALUE) {
        uint32_t error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return RESULT_NOT_FOUND;
        }
        LogError(error, "Failed to open file.");
        if (error < 6) {
            if (error == ERROR_ACCESS_DENIED) {
                return RESULT_ACCESS_DENIED;
            }
            if (error == ERROR_SUCCESS) {
                return RESULT_SUCCESS;
            }
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return RESULT_NOT_FOUND;
            }
        } else {
            if (error == ERROR_HANDLE_EOF) {
                return RESULT_END_OF_FILE;
            }
            if (error == ERROR_OPERATION_ABORTED) {
                return RESULT_ABORTED;
            }
        }
        LogError(error, "Encountered an unexpected result.");
        return RESULT_FAILED;
    }
    CreateIoCompletionPort(handle, context->m_port, KEY_REQUEST, 0);
    return RESULT_SUCCESS;
}

// Unbuffered files need sector-aligned offsets, sizes and buffers: an unaligned read goes
// through a bounce buffer the IO thread copies out of.
// ref: FUN_142168a10
uint64_t FileRead(CONTEXT* context, OSFILE* file, void* buffer, uint64_t offset, uint32_t size, CALLBACK_ callback,
    void* callbackContext) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == INVALID_HANDLE_VALUE) {
        LogError(GetLastError(), "Failed to create overlap completion event.");
        return s_noKey;
    }
    auto request = static_cast<REQUEST*>(context->m_allocator->allocate(sizeof(REQUEST), 8));
    if (request) {
        std::memset(request, 0, sizeof(REQUEST));
    }
    std::memset(&request->m_overlapped, 0, sizeof(request->m_overlapped));
    request->m_file = file->m_handle;
    request->m_event = event;
    request->m_status = ERROR_IO_PENDING;
    request->m_key = NextKey();
    request->m_callback = callback;
    request->m_context = callbackContext;
    request->m_size = size;
    request->m_offset = offset;
    request->m_destination = buffer;
    request->m_bounce = nullptr;
    request->m_38 = 1;
    uint64_t readOffset = offset;
    uint32_t readSize = size;
    void* target = buffer;
    if (!context->m_buffered) {
        readOffset = offset & ~uint64_t(0xFFF);
        readSize = AlignedSize(size, static_cast<uint32_t>(offset), static_cast<uint32_t>(readOffset));
        if (readOffset != offset || readSize != size || (reinterpret_cast<uint64_t>(buffer) & 0xFFF) != 0) {
            target = context->m_allocator->allocate(readSize, 0x1000);
            request->m_bounce = target;
        }
    }
    request->m_overlapped.Offset = static_cast<uint32_t>(readOffset);
    request->m_overlapped.OffsetHigh = static_cast<uint32_t>(readOffset >> 32);
    AcquireSRWLockExclusive(&context->m_lock);
    bool inserted;
    context->m_requests.assign(request->m_key, request, &inserted);
    ReleaseSRWLockExclusive(&context->m_lock);
    uint64_t key = request->m_key;
    DWORD read = 0;
    if (!ReadFile(request->m_file, target, readSize, &read, &request->m_overlapped)) {
        uint32_t error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            if (error != ERROR_HANDLE_EOF) {
                LogError(GetLastError(), "Failed to initiate file read.");
            }
            request->m_status = error;
            request->m_transferred = read;
            PostQueuedCompletionStatus(context->m_port, read, KEY_REQUEST, &request->m_overlapped);
        }
    }
    return key;
}

// ref: FUN_142168590
uint8_t FileResult(CONTEXT* context, uint64_t key, uint32_t* transferred) {
    AcquireSRWLockShared(&context->m_lock);
    auto entry = context->m_requests.find(key);
    if (!entry) {
        ReleaseSRWLockShared(&context->m_lock);
        return RESULT_INVALID_PARAM;
    }
    REQUEST* request = entry->m_value;
    ReleaseSRWLockShared(&context->m_lock);
    if (!request) {
        return RESULT_INVALID_PARAM;
    }
    uint32_t status;
    if (WaitForSingleObject(request->m_event, INFINITE) == WAIT_OBJECT_0) {
        status = request->m_status;
    } else {
        status = GetLastError();
    }
    uint8_t result = ResultFromError(status);
    uint32_t done = request->m_transferred;
    if (request->m_size < done) {
        done = request->m_size;
    }
    *transferred = done;
    if (result == RESULT_SUCCESS && done < request->m_size) {
        result = RESULT_END_OF_FILE;
    }
    AcquireSRWLockExclusive(&context->m_lock);
    entry = context->m_requests.find(key);
    if (entry) {
        CloseHandle(entry->m_value->m_event);
        context->m_allocator->deallocate(entry->m_value);
        context->m_requests.remove(key);
        ReleaseSRWLockExclusive(&context->m_lock);
        return result;
    }
    ReleaseSRWLockExclusive(&context->m_lock);
    return RESULT_INVALID_PARAM;
}

// Whether a request has finished: an unknown key counts as finished.
// ref: FUN_142168730
bool FileIsDone(CONTEXT* context, uint64_t key) {
    AcquireSRWLockShared(&context->m_lock);
    auto entry = context->m_requests.find(key);
    if (entry) {
        REQUEST* request = entry->m_value;
        ReleaseSRWLockShared(&context->m_lock);
        if (!request) {
            return true;
        }
        return WaitForSingleObject(request->m_event, 0) == WAIT_OBJECT_0;
    }
    ReleaseSRWLockShared(&context->m_lock);
    return true;
}

// ref: FUN_142168490
uint8_t FileCancel(CONTEXT* context, uint64_t key) {
    AcquireSRWLockShared(&context->m_lock);
    auto entry = context->m_requests.find(key);
    if (!entry) {
        ReleaseSRWLockShared(&context->m_lock);
        return RESULT_INVALID_PARAM;
    }
    REQUEST* request = entry->m_value;
    ReleaseSRWLockShared(&context->m_lock);
    if (!request) {
        return RESULT_INVALID_PARAM;
    }
    if (request->m_file != INVALID_HANDLE_VALUE) {
        CancelIoEx(request->m_file, &request->m_overlapped);
    }
    return RESULT_SUCCESS;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_142169150 = (void*)&aio::AlignedSize;
extern "C" void* const gen_f_1421691f0 = (void*)&aio::NextKey;
// ---- end of generated globals ----
