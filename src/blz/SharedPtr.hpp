#ifndef BLZ_SHARED_PTR_HPP
#define BLZ_SHARED_PTR_HPP

// blz::shared_ptr and blz::weak_ptr (pkg/blz/include/blz/mem_shared_ptr.h): a pointer and a
// control block, as the standard library lays them out. The control block counts its
// strong references at +0x08 and its weak ones at +0x0c (the strong references together hold
// one weak one); the last strong release disposes of the object (slot +0x10) and the last
// weak release frees the block (slot +0x18).

#include <atomic>
#include <cstdint>

namespace blz {

class shared_count {
    public:
    std::atomic<int32_t> m_uses{1};     // 0x08
    std::atomic<int32_t> m_weak{1};     // 0x0c

    virtual ~shared_count() = default;
    virtual void* get_deleter() = 0;    // +0x08
    virtual void dispose() = 0;         // +0x10: the object goes
    virtual void destroy() = 0;         // +0x18: the block goes

    void add_ref() {
        this->m_uses.fetch_add(1);
    }

    // Takes a strong reference unless the object is already gone.
    bool add_ref_lock() {
        int32_t uses = this->m_uses.load();
        do {
            if (uses == 0) {
                return false;
            }
        } while (!this->m_uses.compare_exchange_weak(uses, uses + 1));
        return true;
    }

    void release() {
        if (this->m_uses.fetch_sub(1) == 1) {
            this->dispose();
            this->weak_release();
        }
    }

    void weak_add_ref() {
        this->m_weak.fetch_add(1);
    }

    void weak_release() {
        if (this->m_weak.fetch_sub(1) == 1) {
            this->destroy();
        }
    }
};
static_assert(sizeof(shared_count) == 0x10, "the control block's counts end at +0x10");

template <class T>
class weak_ptr;

template <class T>
class shared_ptr {
    public:
    T* m_ptr = nullptr;
    shared_count* m_count = nullptr;

    shared_ptr() = default;
    shared_ptr(T* ptr, shared_count* count) : m_ptr(ptr), m_count(count) {}

    shared_ptr(const shared_ptr& other) : m_ptr(other.m_ptr), m_count(other.m_count) {
        if (this->m_count != nullptr) {
            this->m_count->add_ref();
        }
    }

    shared_ptr(shared_ptr&& other) noexcept : m_ptr(other.m_ptr), m_count(other.m_count) {
        other.m_ptr = nullptr;
        other.m_count = nullptr;
    }

    ~shared_ptr() {
        this->reset();
    }

    shared_ptr& operator=(const shared_ptr& other) {
        shared_ptr(other).swap(*this);
        return *this;
    }

    shared_ptr& operator=(shared_ptr&& other) noexcept {
        shared_ptr(static_cast<shared_ptr&&>(other)).swap(*this);
        return *this;
    }

    void reset() {
        shared_count* count = this->m_count;
        this->m_ptr = nullptr;
        this->m_count = nullptr;
        if (count != nullptr) {
            count->release();
        }
    }

    void swap(shared_ptr& other) {
        T* ptr = this->m_ptr;
        shared_count* count = this->m_count;
        this->m_ptr = other.m_ptr;
        this->m_count = other.m_count;
        other.m_ptr = ptr;
        other.m_count = count;
    }

    T* get() const {
        return this->m_ptr;
    }
    T* operator->() const {
        return this->m_ptr;
    }
    T& operator*() const {
        return *this->m_ptr;
    }
    explicit operator bool() const {
        return this->m_ptr != nullptr;
    }
    bool operator==(const shared_ptr& other) const {
        return this->m_ptr == other.m_ptr;
    }
    bool operator!=(const shared_ptr& other) const {
        return this->m_ptr != other.m_ptr;
    }
};
static_assert(sizeof(shared_ptr<int>) == 0x10, "blz::shared_ptr is 0x10 bytes");

template <class T>
class weak_ptr {
    public:
    T* m_ptr = nullptr;
    shared_count* m_count = nullptr;

    weak_ptr() = default;
    weak_ptr(const weak_ptr&) = delete;
    weak_ptr& operator=(const weak_ptr&) = delete;

    ~weak_ptr() {
        this->reset();
    }

    weak_ptr& operator=(const shared_ptr<T>& other) {
        if (other.m_count != nullptr) {
            other.m_count->weak_add_ref();
        }
        shared_count* previous = this->m_count;
        this->m_ptr = other.m_ptr;
        this->m_count = other.m_count;
        if (previous != nullptr) {
            previous->weak_release();
        }
        return *this;
    }

    void reset() {
        shared_count* count = this->m_count;
        this->m_ptr = nullptr;
        this->m_count = nullptr;
        if (count != nullptr) {
            count->weak_release();
        }
    }

    // A strong reference, empty when the object is gone.
    shared_ptr<T> lock() const {
        shared_ptr<T> result;
        if (this->m_count != nullptr && this->m_count->add_ref_lock()) {
            result.m_ptr = this->m_ptr;
            result.m_count = this->m_count;
        }
        return result;
    }
};

}

#endif
