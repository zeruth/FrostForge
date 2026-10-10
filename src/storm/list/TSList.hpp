#ifndef STORM_LIST_TS_LIST_HPP
#define STORM_LIST_TS_LIST_HPP

#include "storm/list/TSLink.hpp"
#include <cstddef>
#include <cstdint>

template <class T>
class TSGetLink {
    public:
    static TSLink<T>* Link(T* ptr, ptrdiff_t linkoffset) {
        return reinterpret_cast<TSLink<T>*>(reinterpret_cast<uintptr_t>(ptr) + linkoffset);
    }
};

// An intrusive list: a link offset and a terminator link. The third template argument
// appears in the reference's type name (TSList<CMDDEF, TSGetLink<CMDDEF>, 8>); its meaning
// is not known yet and nothing here uses it.
//
// Construction matches the reference's static initializers (e.g. 0x1401cb400): the
// terminator's prevlink points at itself (static data) and its m_next is set at startup to
// the tagged terminator (terminator | 1). The destructor is 0x14025cfe0, registered with
// atexit by the same initializer.
template <class T, class TGetLink, int32_t N>
class TSList {
    public:
    ptrdiff_t m_linkoffset = 0;
    TSLink<T> m_terminator;

    TSList() {
        this->m_terminator.m_prevlink = &this->m_terminator;
        this->m_terminator.m_next = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(&this->m_terminator) | 1);
    }

    // A list whose link is not the node's first field (the reference stores the offset as
    // static data beside the terminator).
    explicit TSList(ptrdiff_t linkoffset) : TSList() {
        this->m_linkoffset = linkoffset;
    }

    // ref: FUN_14025cfe0
    ~TSList() {
        while (T* node = this->Head()) {
            TGetLink::Link(node, this->m_linkoffset)->Unlink();
        }
        this->m_terminator.Unlink();
    }

    // ref: FUN_140255610
    T* Head() const {
        return this->m_terminator.Next();
    }

    TSLink<T>* Link(T* ptr) const {
        return TGetLink::Link(ptr, this->m_linkoffset);
    }

    T* Next(T* ptr) const {
        return this->Link(ptr)->Next();
    }

    // Links ptr at the head: right after the terminator (the UI root's list moves inline it).
    void LinkToHead(T* ptr) {
        TSLink<T>* link = this->Link(ptr);
        link->m_prevlink = &this->m_terminator;
        link->m_next = this->m_terminator.m_next;
        this->m_terminator.NextLink(this->m_linkoffset)->m_prevlink = link;
        this->m_terminator.m_next = ptr;
    }

    // Links ptr at the tail: after the terminator's prevlink, as SCmdRegisterArgument does.
    void LinkToTail(T* ptr) {
        TSLink<T>* link = this->Link(ptr);
        TSLink<T>* last = this->m_terminator.m_prevlink;
        link->m_prevlink = last;
        link->m_next = last->m_next;
        last->m_next = ptr;
        this->m_terminator.m_prevlink = link;
    }
};

#endif
