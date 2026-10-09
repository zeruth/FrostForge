#ifndef STORM_LIST_TS_LINK_HPP
#define STORM_LIST_TS_LINK_HPP

#include <cstddef>
#include <cstdint>

// An intrusive list link, as 3.4.3's Storm lays it out.
//
// m_next points at the next node, or at the list's terminator link tagged with the low bit
// (terminator | 1) at the end of the list; 0 means "not linked". This differs from 3.3.5's
// Storm (squall), which marks the end with a non-positive pointer, so squall's TSLink is not
// borrowed. Every operation here is transcribed from code the reference inlines (SCmd's
// list walks, TSList's destructor 0x14025cfe0).
template <class T>
class TSLink {
    public:
    TSLink<T>* m_prevlink = nullptr;
    T* m_next = nullptr;

    bool IsLinked() const {
        return this->m_next != nullptr;
    }

    // The next node, or nullptr at the end (tagged) or when unlinked (0).
    T* Next() const {
        uintptr_t next = reinterpret_cast<uintptr_t>(this->m_next);
        return (next & 1) || next == 0 ? nullptr : this->m_next;
    }

    // The next node's link. A negative linkoffset recovers the offset from the previous
    // link, in 32-bit arithmetic as the reference does it.
    TSLink<T>* NextLink(ptrdiff_t linkoffset) const {
        uintptr_t next = reinterpret_cast<uintptr_t>(this->m_next);
        if ((next & 1) == 0 && next != 0) {
            ptrdiff_t offset = linkoffset;
            if (linkoffset < 0) {
                offset = static_cast<int32_t>(
                    static_cast<int32_t>(reinterpret_cast<uintptr_t>(this))
                    - static_cast<int32_t>(reinterpret_cast<uintptr_t>(this->m_prevlink->m_next)));
            }
            return reinterpret_cast<TSLink<T>*>(next + offset);
        }
        return reinterpret_cast<TSLink<T>*>(next & ~uintptr_t(1));
    }

    void Unlink() {
        if (this->m_prevlink) {
            this->NextLink(-1)->m_prevlink = this->m_prevlink;
            this->m_prevlink->m_next = this->m_next;
            this->m_prevlink = nullptr;
            this->m_next = nullptr;
        }
    }
};

#endif
