#include "console/Var.hpp"
#include "console/Command.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/thread/SCritSect.hpp"
#include "util/Fnv.hpp"
#include "util/FunctionPointer.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <windows.h>

static_assert(offsetof(CVar, m_stringValue) == 0x08, "CVar layout");
static_assert(offsetof(CVar, m_modified) == 0x20, "CVar layout");
static_assert(offsetof(CVar, m_help) == 0x40, "CVar layout");
static_assert(offsetof(CVar, m_name) == 0x50, "CVar layout");
static_assert(offsetof(CVar, m_callback) == 0x78, "CVar layout");
static_assert(sizeof(CVar) == 0x98, "CVar is 0x98 bytes");

namespace {

typedef bool (*CVARINVOKE)(CVar* cvar, const char* oldValue, const char* newValue, void* storage);

// The table of every CVar: blz::unordered_map<t2util::string_hash<ConstFnvHashStringI>,
// blz::unique_ptr<CVar, CVar::Deleter>>. Open addressing over power-of-two buckets with
// triangular probing; a bucket's hash is 0 when empty and 1 when its entry was erased.
struct CVarEntry {
    uint64_t m_hash;
    CVar* m_cvar;
};

const char s_mapTag[] =
    ".?AU?$pair@$$CBV?$string_hash@$1?ConstFnvHashStringI@t2util@@YA_KPEBD@Z$0BAAAAAAABLD@@t2util@@V?$unique_ptr@UCVar@@UDeleter@1@@blz@@@blz@@";

struct CVarMap {
    uint64_t m_bucketCount;  // 0x00
    CVarEntry* m_buckets;    // 0x08
    uint64_t m_size;         // 0x10: entries, erased ones included
    uint64_t m_erased;       // 0x18
    float m_maxLoad;         // 0x20

    // The static initializer: 8 empty buckets, a 0.75 load factor. (The atexit destructor it
    // registers, 0x1425dba70, is not ported.)
    // ref: 0x1401cbc40
    CVarMap() : m_bucketCount(8), m_size(0), m_erased(0) {
        this->m_buckets = static_cast<CVarEntry*>(SMemAlloc(0x80, 0xB, s_mapTag, SStrHashFnv1a(s_mapTag), -6, 0));
        for (uint64_t i = 0; i < this->m_bucketCount; i++) {
            this->m_buckets[i].m_hash = 0;
        }
        this->m_maxLoad = 0.75f;
    }

    // The bucket holding `hash`, or the end bucket. (Inlined at every lookup.)
    CVarEntry* Find(uint64_t hash) {
        uint64_t mask = this->m_bucketCount - 1;
        uint64_t index = hash & mask;
        CVarEntry* entry = &this->m_buckets[index];
        for (uint64_t step = 0; entry->m_hash != 0;) {
            if ((this->m_erased == 0 || entry->m_hash != 1) && entry->m_hash == hash) {
                return entry;
            }
            step++;
            index = (index + step) & mask;
            entry = &this->m_buckets[index];
        }
        return &this->m_buckets[this->m_bucketCount];
    }

    CVarEntry* End() {
        return &this->m_buckets[this->m_bucketCount];
    }

    bool IsLive(const CVarEntry* entry) const {
        return entry->m_hash != 0 && (this->m_erased == 0 || entry->m_hash != 1);
    }

    // Iteration: the first live bucket at or after `entry`. (Inlined.)
    CVarEntry* Skip(CVarEntry* entry) {
        while (entry < this->End() && !this->IsLive(entry)) {
            entry++;
        }
        return entry;
    }

    // The value slot for `hash`, adding an empty one if there is none: the first erased
    // bucket on the probe path is reused, or else the empty bucket that ends it. (Inlined.)
    CVar** Emplace(uint64_t hash) {
        if (this->m_maxLoad < static_cast<float>(this->m_size + 1) / static_cast<float>(this->m_bucketCount)) {
            this->Rehash(0);
        }
        uint64_t mask = this->m_bucketCount - 1;
        uint64_t index = hash & mask;
        CVarEntry* entry = &this->m_buckets[index];
        CVarEntry* reuse = nullptr;
        for (uint64_t step = 0; entry->m_hash != 0;) {
            if (entry->m_hash == 1) {
                if (!reuse) {
                    reuse = entry;
                }
            } else if (entry->m_hash == hash) {
                return &entry->m_cvar;
            }
            step++;
            index = (index + step) & mask;
            entry = &this->m_buckets[index];
        }
        if (reuse) {
            entry = reuse;
        }
        entry->m_hash = hash;
        entry->m_cvar = nullptr;
        this->m_size++;
        return &entry->m_cvar;
    }

    // Resizes to the smallest power of two (at least 8) that holds the live entries under the
    // load factor and is at least `minimum`, dropping erased buckets. Does nothing when the
    // size would not change and nothing is erased.
    // ref: FUN_140322e50
    void Rehash(uint64_t minimum) {
        float live = static_cast<float>(this->m_size - this->m_erased);
        uint64_t needed = static_cast<uint64_t>(live / this->m_maxLoad);
        if (live > static_cast<float>(needed) * this->m_maxLoad) {
            needed++;
        }
        if (needed < minimum) {
            needed = minimum;
        }
        uint64_t count = 8;
        while (count < needed) {
            count *= 2;
        }
        if (count == this->m_bucketCount && this->m_erased == 0) {
            return;
        }

        auto buckets = static_cast<CVarEntry*>(SMemAlloc(count * sizeof(CVarEntry), 0xB, s_mapTag,
            SStrHashFnv1a(s_mapTag), -6, 0));
        for (uint64_t i = 0; i < count; i++) {
            buckets[i].m_hash = 0;
        }

        for (CVarEntry* old = this->m_buckets; old != this->m_buckets + this->m_bucketCount; old++) {
            if (old->m_hash == 0 || (this->m_erased != 0 && old->m_hash == 1)) {
                continue;
            }
            uint64_t index = old->m_hash & (count - 1);
            CVarEntry* entry = &buckets[index];
            for (uint64_t step = 0; entry->m_hash != 0;) {
                step++;
                index = (index + step) & (count - 1);
                entry = &buckets[index];
            }
            entry->m_cvar = old->m_cvar;
            old->m_cvar = nullptr;
            entry->m_hash = old->m_hash;
        }

        for (uint64_t i = 0; i < this->m_bucketCount; i++) {
            if (this->m_buckets[i].m_hash > 1 && this->m_buckets[i].m_cvar) {
                CVar::Deleter()(this->m_buckets[i].m_cvar);
            }
        }
        SMemFree(this->m_buckets, s_mapTag, -6, 0);

        uint64_t erased = this->m_erased;
        this->m_erased = 0;
        this->m_buckets = buckets;
        this->m_size -= erased;
        this->m_bucketCount = count;
    }
};

static_assert(offsetof(CVarMap, m_maxLoad) == 0x20, "CVarMap layout");

CVarMap s_cvars;  // 0x142ccbd68

// DAT_142e260a8: called after a value changes, with the old and new value. The client sets it
// (CVarSetChangedHandler) when it registers its own variables.
void (*s_cvarChanged)(CVar* cvar, const char* oldValue, const char* newValue);

uint8_t s_cvarsDirty;       // DAT_142e260b0: something to write back to Config.wtf
uint8_t s_cvarsInitialized; // DAT_142e260b1
uint8_t s_cvarsLoading;     // DAT_142e260b2: set while a config file is applied (0x140320393..)
uint8_t s_cvarsRegistering; // DAT_142e260b3: set inside CVarRegister
uint8_t s_testCVarChanged;  // DAT_142e260b4

}

SCritSect g_cvarLock;  // DAT_142e260c8, initialized by the console's CVar setup

// unique_ptr<CVar>'s deleter: the console command, the callback, the name, the object.
// ref: FUN_140323250 (the unique_ptr reset this is inlined into)
void CVar::Deleter::operator()(CVar* cvar) const {
    if (cvar->m_name.m_data) {
        ConsoleCommandUnregister(cvar->m_name.m_data);
    }
    cvar->m_callback.m_manager(cvar->m_callback.m_storage, nullptr, 0);
    cvar->~CVar();
    ::operator delete(cvar, sizeof(CVar));
}

namespace {

// unique_ptr<CVar, CVar::Deleter>::reset
// ref: FUN_140322d90
void CVarReset(CVar** slot, CVar* cvar) {
    CVar* old = *slot;
    *slot = cvar;
    if (old) {
        CVar::Deleter()(old);
    }
}

// Runs the callback on a change, after checking that its target is code in this executable.
// The callback's answer is whether the value is accepted.
// ref: FUN_14031ef60
bool CVarCallHandler(CVar* cvar, const char* oldValue, const char* newValue) {
    uint64_t* target = nullptr;
    if (cvar->m_callback.m_invoke) {
        cvar->m_callback.m_manager(cvar->m_callback.m_storage, &target, 3);
    }
    CheckFunctionPointer(*target);
    return reinterpret_cast<CVARINVOKE>(cvar->m_callback.m_invoke)(cvar, oldValue, newValue, cvar->m_callback.m_storage);
}

// The C-callback form's heap block and its invoker and manager.
struct CVarCallback {
    CVARCALLBACK m_function;
    void* m_userData;
    uint8_t m_unused[0x18];  // copied from the caller's frame, never written
};

// ref: FUN_140322e30
bool CVarCallbackInvoke(CVar* cvar, const char* oldValue, const char* newValue, void* storage) {
    auto callback = *static_cast<CVarCallback**>(storage);
    return callback->m_function(cvar, oldValue, newValue, callback->m_userData);
}

// ref: FUN_1403094d0
void CVarCallbackManage(void* dest, void* src, int32_t op) {
    auto destBlock = static_cast<CVarCallback**>(dest);
    auto srcBlock = static_cast<CVarCallback**>(src);
    if (op == 0) {
        SMemFree(*destBlock, nullptr, -6, 0);
    } else if (op == 1) {
        auto copy = static_cast<CVarCallback*>(SMemAllocAligned(sizeof(CVarCallback), 8, 0xB, nullptr, 0, -6, 0));
        if (copy) {
            *copy = **srcBlock;
        }
        *destBlock = copy;
    } else if (op == 2) {
        *destBlock = *srcBlock;
    } else if (op == 3) {
        *srcBlock = *destBlock;
    }
}

// Moves `from` into the CVar's callback, leaving `from` empty.
void CVarTakeCallback(CVar* cvar, blz::function_storage* from) {
    cvar->m_callback.m_manager(cvar->m_callback.m_storage, nullptr, 0);
    from->m_manager(cvar->m_callback.m_storage, from->m_storage, 2);
    cvar->m_callback.m_invoke = from->m_invoke;
    cvar->m_callback.m_manager = from->m_manager;
    from->m_invoke = nullptr;
    from->m_manager = &blz::function_empty_manager;
}

const char* Intern(RCString* string, const char* value) {
    return value ? string->Intern(value) : nullptr;
}

}

// ref: FUN_140321190
CVar* CVarRegister(const char* name, const char* help, uint32_t flags, const char* defaultValue,
    blz::function_storage* callback, uint32_t category, bool isReset, bool a8) {
    if (!name || *name == '\0' || !defaultValue) {
        return nullptr;
    }

    uint8_t registering = s_cvarsRegistering;
    s_cvarsRegistering = 1;
    g_cvarLock.Enter();

    CVar* cvar = nullptr;
    CVarEntry* entry = s_cvars.Find(t2util::ConstFnvHashStringI(name));
    if (entry != s_cvars.End()) {
        cvar = entry->m_cvar;
    }

    if (!cvar) {
        if (ConsoleCommandExists(name)) {
            ConsoleWriteFormat(0, "Attempted to override command with a cvar: %s\n", name);
        } else {
            CVar** slot = s_cvars.Emplace(t2util::ConstFnvHashStringI(name));

            auto created = new (0xB, __FILE__, SStrHashFnv1a(__FILE__), 0x3D8) CVar;
            created->m_category = 5;
            created->m_flags = 0;
            created->m_floatValue = 0.0f;
            created->m_intValue = 0;
            created->m_int64Value = 0;
            created->m_modified = 0;
            created->m_nameHash = 0;
            created->m_callback.m_invoke = nullptr;
            created->m_callback.m_manager = &blz::function_empty_manager;
            CVarReset(slot, created);
            cvar = *slot;

            cvar->m_nameHash = t2util::ConstFnvHashStringI(name);
            blz::string temp;
            temp.assign(name);
            cvar->m_name = static_cast<blz::string&&>(temp);

            cvar->m_category = category;
            CVarTakeCallback(cvar, callback);
            cvar->m_flags = flags & (CVAR_FLAG_DEFERCALL | CVAR_FLAG_NOCALLLOAD);
            cvar->m_help.m_str = Intern(&cvar->m_help, help);

            CVarSet(cvar, defaultValue, true, isReset, !isReset, (flags >> 11) & 1);
            if (!cvar->m_stringValue.m_str) {
                ConsoleWriteFormat(0, "CVar '%s' failed validation for its initial value.", cvar->m_name.m_data);
            }

            if (!(flags & CVAR_FLAG_400)) {
                flags |= CVAR_FLAG_1;
            }
            cvar->m_flags |= flags;
            if (!isReset) {
                cvar->m_flags |= CVAR_FLAG_NOTDEFAULT;
            }
            if (a8 && !(cvar->m_flags & CVAR_FLAG_NOTDEFAULT)) {
                cvar->m_flags |= CVAR_FLAG_80;
            }

            ConsoleCommandRegister(cvar->m_name.m_data, &CVarConsoleHandler, category, cvar->m_help.m_str, 0);
        }
    } else {
        if ((cvar->m_flags & CVAR_FLAG_400) && !(flags & CVAR_FLAG_1)) {
            flags |= CVAR_FLAG_400;
        }
        cvar->m_flags = flags;
        if (!(flags & CVAR_FLAG_400)) {
            cvar->m_flags = flags | CVAR_FLAG_1;
        }

        CVarTakeCallback(cvar, callback);
        cvar->m_help.m_str = Intern(&cvar->m_help, help);

        const char* reset = cvar->m_resetValue.m_str;
        const char* coded = cvar->m_defaultValue.m_str;
        bool rejected = false;
        if (cvar->m_callback.m_invoke && !(flags & CVAR_FLAG_NOCALLLOAD)) {
            const char* value = cvar->m_stringValue.m_str;
            rejected = !reinterpret_cast<CVARINVOKE>(cvar->m_callback.m_invoke)(cvar, value, value, cvar->m_callback.m_storage);
        }
        CVarSet(cvar, defaultValue, rejected, reset == nullptr, coded == nullptr, (flags & CVAR_FLAG_SAVE) || rejected);

        if (!isReset) {
            cvar->m_flags |= CVAR_FLAG_NOTDEFAULT;
        }
        if (a8 && !(cvar->m_flags & CVAR_FLAG_NOTDEFAULT)) {
            cvar->m_flags |= CVAR_FLAG_80;
        }
    }

    g_cvarLock.Leave();
    s_cvarsRegistering = registering;
    return cvar;
}

// ref: FUN_140320f70
CVar* CVarRegister(const char* name, const char* help, uint32_t flags, const char* defaultValue, CVARCALLBACK callback,
    uint32_t category, bool isReset, void* userData, bool a9) {
    blz::function_storage function;
    if (!callback) {
        function.m_invoke = nullptr;
        function.m_manager = &blz::function_empty_manager;
    } else {
        function.m_invoke = reinterpret_cast<void*>(&CVarCallbackInvoke);
        function.m_manager = &CVarCallbackManage;
        auto block = static_cast<CVarCallback*>(SMemAllocAligned(sizeof(CVarCallback), 8, 0xB, nullptr, 0, -6, 0));
        if (block) {
            block->m_function = callback;
            block->m_userData = userData;
            std::memset(block->m_unused, 0, sizeof(block->m_unused));
        }
        std::memcpy(function.m_storage, &block, sizeof(block));
    }

    CVar* cvar = CVarRegister(name, help, flags, defaultValue, &function, category, isReset, a9);

    if (function.m_manager == &CVarCallbackManage) {
        CVarCallback* block;
        std::memcpy(&block, function.m_storage, sizeof(block));
        SMemFree(block, nullptr, -6, 0);
    } else {
        function.m_manager(function.m_storage, nullptr, 0);
    }
    return cvar;
}

// ref: FUN_140322180
bool CVarSet(CVar* cvar, const char* value, bool setValue, bool setReset, bool setDefault, bool markDirty) {
    char* oldValue = nullptr;
    bool callAfter = false;

    if (cvar->m_stringValue.m_str) {
        uint64_t length = std::strlen(cvar->m_stringValue.m_str);
        oldValue = static_cast<char*>(SMemAlloc(length + 1, 1, __FILE__, SStrHashFnv1a(__FILE__), 0x43A, 0));
        std::memcpy(oldValue, cvar->m_stringValue.m_str, length + 1);
    }

    if (setValue) {
        if (cvar->m_callback.m_invoke) {
            if (!(cvar->m_flags & CVAR_FLAG_NOCALLLOAD) || (!s_cvarsLoading && !s_cvarsRegistering)) {
                if (cvar->m_flags & CVAR_FLAG_DEFERCALL) {
                    callAfter = true;
                } else if (!CVarCallHandler(cvar, cvar->m_stringValue.m_str, value)) {
                    goto done;
                }
            }
        }
        cvar->m_modified++;
        if (cvar->m_flags & CVAR_FLAG_LATCHED) {
            cvar->m_latchedValue.m_str = Intern(&cvar->m_latchedValue, value);
            s_cvarsDirty = 1;
            goto done;
        }
    }

    CVarUpdate(cvar, value, setValue, setReset, setDefault, markDirty);
    if (s_cvarChanged) {
        if (!oldValue || std::strcmp(oldValue, value) != 0) {
            s_cvarChanged(cvar, oldValue, value);
        }
    }
    if (callAfter) {
        CVarCallHandler(cvar, oldValue, cvar->m_stringValue.m_str);
    }

done:
    if (oldValue) {
        SMemFree(oldValue, __FILE__, 0x441, 0);
    }
    return true;
}

// ref: FUN_14031ff30
void CVarUpdate(CVar* cvar, const char* value, bool setValue, bool setReset, bool setDefault, bool markDirty) {
    bool changed = false;
    if (cvar->m_flags & CVAR_FLAG_LOCKED) {
        return;
    }
    if (!value) {
        return;
    }

    if (setValue) {
        changed = !cvar->m_stringValue.m_str || std::strcmp(value, cvar->m_stringValue.m_str) != 0;
        if (changed) {
            cvar->m_stringValue.m_str = cvar->m_stringValue.Intern(value);
            bool hex = value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
            cvar->m_intValue = std::strtol(value, nullptr, hex ? 16 : 10);
            cvar->m_int64Value = _strtoi64(value, nullptr, hex ? 16 : 10);
            cvar->m_floatValue = SStrToFloat(value);
        }
    }

    if (setReset) {
        changed = true;
        cvar->m_resetValue.m_str = cvar->m_resetValue.Intern(value);
    }
    if (setDefault) {
        cvar->m_defaultValue.m_str = cvar->m_defaultValue.Intern(value);
    } else if (!changed) {
        return;
    }

    if (markDirty) {
        s_cvarsDirty = 1;
    }
    if ((cvar->m_flags & CVAR_FLAG_TEST) && cvar->m_defaultValue.m_str) {
        if (std::strcmp(value, cvar->m_defaultValue.m_str) != 0) {
            s_testCVarChanged = 1;
            ConsoleWriteFormat(0, "Test CVar %s has been set to %s (default %s)", cvar->m_name.m_data, value,
                cvar->m_defaultValue.m_str);
        }
    }
}

// ref: FUN_140320970
CVar* CVarLookup(uint64_t nameHash) {
    g_cvarLock.Enter();
    CVar* cvar = nullptr;
    CVarEntry* entry = s_cvars.Find(nameHash);
    if (entry != s_cvars.End()) {
        cvar = entry->m_cvar;
    }
    g_cvarLock.Leave();
    return cvar;
}

// ref: FUN_1403200e0
bool CVarIsDefault(CVar* cvar) {
    return cvar->m_defaultValue.m_str && std::strcmp(cvar->m_stringValue.m_str, cvar->m_defaultValue.m_str) == 0;
}

// ref: FUN_14031efe0
int32_t CVarConsoleHandler(const char* command, const char* arguments) {
    uint64_t hash = command ? t2util::ConstFnvHashStringI(command) : 0;
    CVar* cvar = CVarLookup(hash);
    if (cvar->m_flags & CVAR_FLAG_HIDDEN) {
        return 1;
    }

    while (*arguments == ' ') {
        arguments++;
    }

    if (*arguments != '\0') {
        if (cvar->m_flags & (CVAR_FLAG_READONLY | CVAR_FLAG_SECURE)) {
            ConsoleWriteFormat(0, "%s is read only or secure.", cvar->m_name.m_data);
        } else {
            CVarSet(cvar, arguments, true, true, false, true);
        }
        return 1;
    }

    const char* value = cvar->m_stringValue.m_str;
    if (cvar->m_flags & CVAR_FLAG_PASSWORD) {
        const char* current = value ? value : "";
        const char* coded = cvar->m_defaultValue.m_str ? cvar->m_defaultValue.m_str : "";
        if (std::strcmp(current, coded) == 0 && *coded != '\0') {
            value = "(default)";
        } else {
            value = *current != '\0' ? "(configured)" : "";
        }
    }
    if (!value) {
        return 1;
    }

    ConsoleWriteFormat(0, "CVar \"%s\" is \"%s\"", command, value);
    if (cvar->m_flags & CVAR_FLAG_PASSWORD) {
        ConsoleWrite(10, "  This is a Password cvar (Actual value will not be displayed)");
    } else {
        if (cvar->m_defaultValue.m_str && !CVarIsDefault(cvar)) {
            ConsoleWriteFormat(0, "  default value \"%s\"", cvar->m_defaultValue.m_str);
        }
        if (cvar->m_resetValue.m_str && std::strcmp(value, cvar->m_resetValue.m_str) != 0) {
            ConsoleWriteFormat(0, "  reset value \"%s\"", cvar->m_resetValue.m_str);
        }
        if (cvar->m_latchedValue.m_str && std::strcmp(value, cvar->m_latchedValue.m_str) != 0) {
            ConsoleWriteFormat(0, "  pending value \"%s\"", cvar->m_latchedValue.m_str);
        }
    }
    if (cvar->m_help.m_str && *cvar->m_help.m_str != '\0') {
        ConsoleWrite(0, cvar->m_help.m_str);
    }
    return 1;
}

// Back to the startup (reset) value, or the coded default when there is none, through the
// callback.
// ref: FUN_140321930
void CVarResetToStartup(CVar* cvar) {
    const char* value = cvar->m_resetValue.m_str;
    if (!value && !(value = cvar->m_defaultValue.m_str)) {
        return;
    }
    if (cvar->m_callback.m_invoke) {
        uint64_t* target;
        cvar->m_callback.m_manager(cvar->m_callback.m_storage, &target, 3);
        CheckFunctionPointer(*target);
        if (!reinterpret_cast<CVARINVOKE>(cvar->m_callback.m_invoke)(cvar, cvar->m_stringValue.m_str, value,
                cvar->m_callback.m_storage)) {
            return;
        }
    }
    // inlined: CVarUpdate(cvar, value, true, false, false, true)
    CVarUpdate(cvar, value, true, false, false, true);
}

// Back to the coded default, or the startup value when there is none, through the callback.
// ref: FUN_14031f850
void CVarResetToDefault(CVar* cvar) {
    const char* value = cvar->m_defaultValue.m_str;
    if (!value && !(value = cvar->m_resetValue.m_str)) {
        return;
    }
    if (cvar->m_callback.m_invoke) {
        uint64_t* target;
        cvar->m_callback.m_manager(cvar->m_callback.m_storage, &target, 3);
        CheckFunctionPointer(*target);
        if (!reinterpret_cast<CVARINVOKE>(cvar->m_callback.m_invoke)(cvar, cvar->m_stringValue.m_str, value,
                cvar->m_callback.m_storage)) {
            return;
        }
    }
    // inlined: CVarUpdate(cvar, value, true, false, false, true)
    CVarUpdate(cvar, value, true, false, false, true);
}

namespace {

const char s_tokenSeparators[] = " ,;\t\"\r\n";

uint64_t HashToken(const char* token) {
    return t2util::ConstFnvHashStringI(token);
}

// Appends `source` to the string in a 0x100-byte buffer, truncating. (Inlined in cvarlist.)
void AppendLine(char* line, const char* source) {
    char* end = line;
    uint64_t left = 0x100;
    while (left != 0 && *end != '\0') {
        end++;
        left--;
    }
    if (left == 0) {
        return;
    }
    for (; *source != '\0'; source++) {
        if (left != 1) {
            *end++ = *source;
            left--;
        }
    }
    *end = '\0';
}

// "set <cvar> <value>": sets a CVar, registering it (empty help, category 5) if it does not
// exist. Hidden, read-only and secure CVars are only set while a config file loads.
// ref: FUN_140322380
int32_t CVarSetCommand(const char* command, const char* arguments) {
    (void)command;
    char name[0x40];
    char value[0x800];
    SStrTokenize(&arguments, name, sizeof(name), s_tokenSeparators, nullptr);
    SStrTokenize(&arguments, value, sizeof(value), s_tokenSeparators, nullptr);

    CVar* cvar = CVarLookup(HashToken(name));
    if (!cvar) {
        blz::function_storage function;
        function.m_invoke = nullptr;
        function.m_manager = &blz::function_empty_manager;
        CVarRegister(name, "", 0, value, &function, 5, true, false);
        if (function.m_manager != &blz::function_empty_manager) {
            function.m_manager(function.m_storage, nullptr, 0);
        }
    } else if (s_cvarsLoading
        || (!(cvar->m_flags & CVAR_FLAG_HIDDEN) && !(cvar->m_flags & (CVAR_FLAG_READONLY | CVAR_FLAG_SECURE)))) {
        CVarSet(cvar, value, true, false, false, true);
    }
    return 1;
}

// "cvar_reset [cvar]": one CVar, or all of them, back to the startup value.
// ref: FUN_14031f6b0
int32_t CVarResetCommand(const char* command, const char* arguments) {
    (void)command;
    char name[0x40];
    SStrTokenize(&arguments, name, sizeof(name), s_tokenSeparators, nullptr);
    if (name[0] != '\0') {
        CVar* cvar = CVarLookup(HashToken(name));
        if (cvar && !(cvar->m_flags & CVAR_FLAG_HIDDEN)) {
            CVarResetToStartup(cvar);
        } else {
            ConsoleWriteFormat(3, "No such cvar \"%s\"\n", name);
        }
        return 1;
    }

    ConsoleWriteText("Resetting all cvars\n");
    g_cvarLock.Enter();
    for (CVarEntry* entry = s_cvars.Skip(s_cvars.m_buckets); entry != s_cvars.End(); entry = s_cvars.Skip(entry + 1)) {
        CVarResetToStartup(entry->m_cvar);
    }
    g_cvarLock.Leave();
    return 1;
}

// "cvar_default [cvar]": one CVar, or all of them, back to the coded default.
// ref: FUN_14031f2b0
int32_t CVarDefaultCommand(const char* command, const char* arguments) {
    (void)command;
    char name[0x40];
    SStrTokenize(&arguments, name, sizeof(name), s_tokenSeparators, nullptr);
    if (name[0] != '\0') {
        CVar* cvar = CVarLookup(HashToken(name));
        if (cvar && !(cvar->m_flags & CVAR_FLAG_HIDDEN)) {
            CVarResetToDefault(cvar);
        } else {
            ConsoleWriteFormat(3, "No such cvar \"%s\"\n", name);
        }
        return 1;
    }

    ConsoleWriteText("Restoring all cvars\n");
    g_cvarLock.Enter();
    for (CVarEntry* entry = s_cvars.Skip(s_cvars.m_buckets); entry != s_cvars.End(); entry = s_cvars.Skip(entry + 1)) {
        CVarResetToDefault(entry->m_cvar);
    }
    g_cvarLock.Leave();
    return 1;
}

// "cvarlist": every visible CVar with its value, and its default and reset values where the
// value differs from them.
// ref: FUN_14031f450
int32_t CVarListCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    char extra[0x100];
    char line[0x100];

    g_cvarLock.Enter();
    for (CVarEntry* entry = s_cvars.Skip(s_cvars.m_buckets); entry != s_cvars.End(); entry = s_cvars.Skip(entry + 1)) {
        CVar* cvar = entry->m_cvar;
        if (cvar->m_flags & CVAR_FLAG_HIDDEN) {
            continue;
        }
        const char* value = cvar->m_stringValue.m_str;
        SStrPrintf(line, sizeof(line), "  \"%s\" is \"%s\"", cvar->m_name.m_data, value);
        if (cvar->m_defaultValue.m_str && std::strcmp(value, cvar->m_defaultValue.m_str) != 0) {
            SStrPrintf(extra, sizeof(extra), " (default \"%s\")", cvar->m_defaultValue.m_str);
            AppendLine(line, extra);
        }
        if (cvar->m_resetValue.m_str && std::strcmp(value, cvar->m_resetValue.m_str) != 0) {
            SStrPrintf(extra, sizeof(extra), " (reset \"%s\")", cvar->m_resetValue.m_str);
            AppendLine(line, extra);
        }
        ConsoleWriteText(line);
    }
    g_cvarLock.Leave();
    return 1;
}

}

namespace {

// DAT_142e301d0: the name of the config file CVarLoadConfig read.
char* s_configFile;

}

// Applies a config file's lines: the bracketed section headers go to `sectionCallback` (no
// callback skips every section after the first header), and the other lines run through the
// console - only "SET" lines unless executeAll. A UTF-8 byte order mark is skipped. False
// when the file cannot be read.
// ref: FUN_140320480
bool CVarParseFile(void* file, CVARSECTIONCALLBACK sectionCallback, void* param, bool executeAll) {
    s_cvarsLoading = 1;
    bool result = true;
    uint32_t size = static_cast<uint32_t>(OsGetFileSize(file, 0));
    bool active = true;
    if (size != 0) {
        char* heap = nullptr;
        char* text;
        if (size < 0x1000) {
            text = static_cast<char*>(_alloca((uint64_t(size) + 1 + 0xF) & ~uint64_t(0xF)));
        } else {
            heap = static_cast<char*>(SMemAlloc(uint64_t(size) + 1, 1, __FILE__, SStrHashFnv1a(__FILE__), 0x17F, 0));
            text = heap;
        }

        uint32_t read;
        if (!OsReadFile(file, text, size, &read)) {
            result = false;
        } else {
            const char* cur = text;
            text[size] = '\0';
            if (text && read > 2 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB
                && static_cast<uint8_t>(text[2]) == 0xBF) {
                cur = text + 3;
            }
            do {
                char line[0x800];
                SStrTokenize(&cur, line, sizeof(line), "\r\n", nullptr);
                if (line[0] == '[') {
                    active = sectionCallback ? sectionCallback(line, param) : false;
                } else if (active) {
                    if (executeAll || _strnicmp(line, "SET ", 4) == 0) {
                        ConsoleCommandExecute(line, 0);
                    }
                }
            } while (cur && *cur != '\0');
        }
        SMemFree(heap, __FILE__, 0x19E, 0);
    }
    s_cvarsLoading = 0;
    return result;
}

// Opens a config file by name, or failing that WTF\<name>, and applies it.
// ref: FUN_140320690
bool CVarExecFile(const char* name, CVARSECTIONCALLBACK sectionCallback, void* param, bool executeAll) {
    void* file = OsCreateFile(name, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (file == INVALID_HANDLE_VALUE) {
        char path[0x110];
        SStrPrintf(path, 0x104, "WTF\\%s", name);
        file = OsCreateFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
    }
    bool result = CVarParseFile(file, sectionCallback, param, executeAll);
    OsCloseFile(file);
    return result;
}

// Remembers the config file's name and applies it.
// ref: FUN_1403268b0
void CVarLoadConfig(const char* name) {
    s_configFile = SStrDupA(name, nullptr, 0);
    CVarExecFile(name, nullptr, nullptr, false);
}

// Applies a config file and marks the CVars dirty, so the result is written back.
// ref: FUN_140326900
void CVarExecFileDirty(const char* name) {
    s_cvarsDirty = 1;
    CVarExecFile(name, nullptr, nullptr, false);
}

// Makes a latched CVar's pending value its value (without the callback). False when the
// CVar is not latched or nothing is pending.
// ref: FUN_140322af0
bool CVarApplyLatched(CVar* cvar) {
    if (!(cvar->m_flags & CVAR_FLAG_LATCHED) || !cvar->m_latchedValue.m_str) {
        return false;
    }
    const char* value = cvar->m_latchedValue.m_str;
    if (!(cvar->m_flags & CVAR_FLAG_LOCKED)) {
        // inlined: CVarUpdate(cvar, value, true, false, false, true)
        CVarUpdate(cvar, value, true, false, false, true);
    }
    cvar->m_latchedValue.m_str = nullptr;
    return true;
}

// ref: FUN_140322110
void CVarSetInt(CVar* cvar, int32_t value, bool setValue, bool setReset, bool setDefault, bool markDirty) {
    char text[0x20];
    SStrPrintf(text, sizeof(text), "%d", value);
    CVarSet(cvar, text, setValue, setReset, setDefault, markDirty);
}

// ref: FUN_140322540
void CVarSetFloat(CVar* cvar, float value, bool setValue, bool setReset, bool setDefault, bool markDirty) {
    char text[0x20];
    SStrPrintf(text, sizeof(text), "%f", static_cast<double>(value));
    CVarSet(cvar, text, setValue, setReset, setDefault, markDirty);
}

// The coded default, unless the variable keeps its own (flag 4).
// ref: FUN_1403224f0
void CVarSetDefaultInt(CVar* cvar, int64_t value) {
    if ((cvar->m_flags & 4) != 0) {
        return;
    }
    char text[0x20];
    SStrPrintf(text, sizeof(text), "%lli", value);
    cvar->m_defaultValue.Intern(text);
}

// ref: FUN_140322640
void CVarSetChangedHandler(void (*handler)(CVar* cvar, const char* oldValue, const char* newValue)) {
    s_cvarChanged = handler;
}

// Sets up the CVar system: the lock, optionally the directories down to <cwd>\WTF (each
// directory above it; WTF itself is left for whoever writes there), and the four commands.
// ref: FUN_14031fd90
void CVarInitialize(bool createWtfDirectory) {
    s_cvarsInitialized = 1;
    g_cvarLock.Initialize();

    if (createWtfDirectory) {
        char cwd[0x110];
        char path[0x110];
        char part[0x110];
        std::memset(cwd, 0, 0x104);
        OsGetWorkingDirectory(0x104, cwd);
        OsPathCombine(path, 0x104, cwd, "WTF");
        if (path[0] != '\0') {
            for (const char* separator = std::strchr(path + 1, '\\'); separator; separator = std::strchr(separator + 1, '\\')) {
                int64_t length = separator - path;
                if (length >= 0x104) {
                    break;
                }
                std::memcpy(part, path, length);
                part[length] = '\0';
                if (!OsCreateDirectory(part, false, false)) {
                    break;
                }
            }
        }
    }

    ConsoleCommandRegister("set", &CVarSetCommand, 5, "Set the value of a CVar", 0);
    ConsoleCommandRegister("cvar_reset", &CVarResetCommand, 5, "Set the value of a CVar to its startup value", 0);
    ConsoleCommandRegister("cvar_default", &CVarDefaultCommand, 5, "Set the value of a CVar to its coded default value", 0);
    ConsoleCommandRegister("cvarlist", &CVarListCommand, 5, "List CVars", 0);
}

// ref: FUN_140322c70
void CVarConvertBitfield(CVar* cvar, int32_t marker) {
    if (cvar == nullptr || marker != 1) {
        return;
    }
    uint8_t out[0x100 + 8];
    const uint8_t* source = reinterpret_cast<const uint8_t*>(cvar->m_stringValue.GetString()) + 1;
    int32_t bit = 0;
    int32_t used = 0;
    uint32_t index = 1;
    out[0] = 2;
    while (*source != 0 && index < 0xff) {
        uint8_t* slot = &out[index];
        if (bit == 0) {
            *slot = 0;
        }
        int32_t shift = bit - used;
        uint32_t count = static_cast<uint32_t>(shift) & 0x1f;
        *slot |= count >= 8 ? 0 : static_cast<uint8_t>((*source & 0x7f) << count);
        *slot = static_cast<uint8_t>((*slot & 0x3f) | 0x40);
        if (used > bit + 1) {
            source++;
            bit = shift + 7;
            used = 0;
        } else {
            index++;
            if (used < bit + 1) {
                used += 6 - bit;
            } else {
                source++;
                used = 0;
            }
            bit = 0;
        }
    }
    if (bit != 0) {
        out[index + 1] = 0;
    } else {
        out[index] = 0;
    }
    CVarSet(cvar, reinterpret_cast<const char*>(out), true, false, false, true);
}

// ref: FUN_140320b40
bool CVarGetBit(CVar* cvar, uint32_t index) {
    if (cvar == nullptr) {
        return false;
    }
    const char* value = cvar->m_stringValue.GetString();
    if (value == nullptr || *value == '\0') {
        return false;
    }
    uint64_t length = std::strlen(value);
    if (static_cast<uint64_t>(index) >= length * 6 - 6) {
        return false;
    }
    return ((static_cast<int32_t>(value[index / 6 + 1]) >> (index % 6)) & 1) != 0;
}

// ref: FUN_140320bb0
void CVarSetBit(CVar* cvar, uint32_t index, bool on) {
    if (cvar == nullptr) {
        return;
    }
    const char* value = cvar->m_stringValue.GetString();
    uint64_t length = std::strlen(value);
    uint32_t count = static_cast<uint32_t>(length);
    if (count >= 0x100) {
        return;
    }
    uint8_t buffer[0x108];
    std::memcpy(buffer, value, length + 1);
    buffer[length] = 0;
    buffer[0] = 2;
    uint32_t slot = index / 6 + 1;
    if (slot >= 0xff) {
        return;
    }
    if (!on) {
        if (slot < count) {
            buffer[slot] = static_cast<uint8_t>(buffer[slot] & ~(1u << (index % 6)));
            while (--count != 0 && buffer[count] == 0x40) {
                buffer[count] = 0;
            }
            CVarSet(cvar, reinterpret_cast<const char*>(buffer), true, false, false, true);
        }
        return;
    }
    if (count <= slot) {
        uint32_t i = count == 0 ? 1 : count;
        while (i <= slot) {
            buffer[i++] = 0x40;
        }
        buffer[index / 6 + 2] = 0;
    }
    buffer[slot] = static_cast<uint8_t>(buffer[slot] | (1u << (index % 6)));
    CVarSet(cvar, reinterpret_cast<const char*>(buffer), true, false, false, true);
}

// ref: FUN_140320110
bool CVarBitIndexValid(uint32_t index) {
    return index / 6 + 1 < 0xff;
}
