#ifndef TACT_ASYNC_STATE_HPP
#define TACT_ASYNC_STATE_HPP

// pkg/tact/3.6.39/src/async_state: the base every asynchronous operation derives from. An
// operation is reference counted, registered in the instance's op registry under its id, and
// finishes by storing a result and calling its completion callback.

#include <cstdint>

namespace tact {

class Instance;

// Error codes; tact::ErrorString names them.
enum ERRORCODE : uint8_t {
    ERR_SUCCESS = 0,
    ERR_UNKNOWN = 1,
    ERR_CANCELLED = 2,
    ERR_INVALID_PARAMETER = 3,
    ERR_BUFFER_TOO_SMALL = 4,
    ERR_NOT_SUPPORTED = 5,
    ERR_NOT_FOUND = 6,
    ERR_NOT_AVAILABLE = 7,
    ERR_DATA = 8,
    ERR_ALREADY_EXISTS = 9,
    ERR_DISK_FULL = 10,
    ERR_NEEDS_REPAIR = 11,
    ERR_WOULD_BLOCK = 12,
    ERR_MISSING_KEY = 13,
    ERR_TRUNCATED = 14,
    ERR_INVALID_DRIVE = 15,
    ERR_CONTAINER_LOCKED = 16,
    ERR_CDN_EXHAUSTED = 17,
    ERR_NEEDS_UPDATE = 18,
    ERR_WOULD_NOT_PIPELINE = 19,
    ERR_BAD_ARMADILLO_KEY = 20,
    ERR_INSUFFICIENT_QUALITY = 21,
    ERR_PATH_TOO_LONG = 22,
};

const char* ErrorString(uint8_t error);

// Called when an operation completes: (context, instance, op id).
typedef void (*OPCALLBACK)(void* context, Instance* instance, uint64_t opId);

// Spin locks on a 64-bit word. Taken exclusively it holds the owner's thread id + 1, so the
// owner taking it again gets null back (and must not release it); taken shared it holds minus
// the number of readers.
int64_t* SpinLockExclusive(int64_t* lock);
int64_t* SpinLockShared(int64_t* lock);

// The base (vtable 0x1428e4ca8), 0x40 bytes.
class AsyncState {
    public:
    int32_t m_refCount;        // 0x08
    Instance* m_instance;      // 0x10
    uint64_t m_opId;           // 0x18
    OPCALLBACK m_callback;     // 0x20
    void* m_context;           // 0x28
    uint8_t m_result;          // 0x30
    uint8_t m_pending;         // 0x31
    uint8_t m_cancelled;       // 0x32
    int64_t m_lock;            // 0x38

    AsyncState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
    virtual ~AsyncState();

    // +0x08: destroys the object and returns it to the allocator it came from.
    virtual void Destroy() = 0;
    // +0x10: what kind of operation this is.
    virtual uint8_t GetType() = 0;
    // +0x18: a detail code beside the result; none by default.
    virtual int32_t GetDetail();
    // +0x20
    virtual void OnStart();
    // +0x28: the operation was cancelled while pending.
    virtual void OnCancel();

    void AddRef();
    void Release();
    void Complete(uint8_t result);
    void Cancel();
};

// A referenced operation with its lock held (or null when the holder already owned it).
struct OPHANDLE {
    AsyncState* m_state;
    int64_t* m_lock;
};

// Holds a reference and the operation's lock for the scope of a callback, as every callback
// prologue/epilogue does.
class AsyncStateScope {
    public:
    AsyncState* m_state;
    int64_t* m_lock;

    explicit AsyncStateScope(AsyncState* state);
    ~AsyncStateScope();
};

}

#endif
