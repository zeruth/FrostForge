#ifndef STORM_HASH_TS_HASH_TABLE_HPP
#define STORM_HASH_TS_HASH_TABLE_HPP

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/list/TSLink.hpp"
#include <cstdint>
#include <cstring>

// Storm's hash table as 3.4.3 builds it, from TSHashTable<CONSOLECOMMAND, HASHKEY_CONSTSTRI, 8>
// (vtable 0x142635d68; static constructor 0x1401cbd00). Every node sits in two lists: its
// bucket (link at +0x00, linked at the HEAD) and the table's full list (link at +0x10,
// linked at the TAIL).
//
// Layout (0x50):
//   +0x00 vtable
//   +0x08 full list: int32 link offset (0x10) + terminator link
//   +0x20 node count
//   +0x28 bucket array: count, ..., data (0x18-byte explicit lists), growth granularity
//   +0x48 bucket modulus (-1 until the first insert; hash % modulus picks the bucket)
//   +0x4C bucket limit (0x80000000)

// An explicit list: a 32-bit link offset and a terminator link. 0xDDDDDDDD (-572662307) is
// the reference's "offset not known yet" value; a negative offset is recovered from the
// neighbouring link.
template <class T>
struct TSExplicitList {
    int32_t m_linkoffset;
    TSLink<T> m_terminator;

    void InitEmpty(int32_t linkoffset) {
        this->m_linkoffset = linkoffset;
        this->m_terminator.m_prevlink = &this->m_terminator;
        this->m_terminator.m_next = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(&this->m_terminator) | 1);
    }

    T* Head() const {
        return this->m_terminator.Next();
    }

    TSLink<T>* Link(T* node) const {
        return reinterpret_cast<TSLink<T>*>(reinterpret_cast<uintptr_t>(node) + this->m_linkoffset);
    }

    // ref: FUN_140325700
    void UnlinkAll() {
        while (T* node = this->Head()) {
            this->Link(node)->Unlink();
        }
    }

    // link after the terminator
    void LinkToHead(T* node) {
        TSLink<T>* link = this->Link(node);
        link->Unlink();
        link->m_prevlink = &this->m_terminator;
        link->m_next = this->m_terminator.m_next;
        uintptr_t next = reinterpret_cast<uintptr_t>(this->m_terminator.m_next);
        if ((next & 1) == 0 && next != 0) {
            int32_t offset = this->m_linkoffset;
            if (offset < 0) {
                offset = static_cast<int32_t>(static_cast<int32_t>(reinterpret_cast<uintptr_t>(&this->m_terminator))
                    - static_cast<int32_t>(reinterpret_cast<uintptr_t>(this->m_terminator.m_prevlink->m_next)));
            }
            reinterpret_cast<TSLink<T>*>(next + offset)->m_prevlink = link;
        } else {
            reinterpret_cast<TSLink<T>*>(next & ~uintptr_t(1))->m_prevlink = link;
        }
        this->m_terminator.m_next = node;
    }

    // link before the terminator
    void LinkToTail(T* node) {
        TSLink<T>* link = this->Link(node);
        link->Unlink();
        TSLink<T>* last = this->m_terminator.m_prevlink;
        link->m_prevlink = last;
        link->m_next = last->m_next;
        last->m_next = node;
        this->m_terminator.m_prevlink = link;
    }
};

// The fields every node starts with.
template <class T>
struct TSHashObject {
    TSLink<T> m_linktoslot;   // 0x00
    TSLink<T> m_linktofull;   // 0x10
    void* m_table;            // 0x20
    uint32_t m_hashval;       // 0x28
};

// HASHKEY_CONSTSTRI: the caller's string, by pointer, compared case-insensitively.
struct HASHKEY_CONSTSTRI {
    const char* m_str;        // 0x30
};

// The allocator tags a table's bucket arrays are freed and grown with: the reference names
// each instantiation's TSFixedArray and TSGrowableArray by type. Specialize per node type.
template <class T>
struct TSHashTableTags {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@UCONSOLECOMMAND@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@UCONSOLECOMMAND@@$0?CCCCCCCD@$07@@$07@@";
};

// LinkBase is where the node's TSHashObject starts: 0 for most tables, 0x10 when the node
// is a handle object (vtable and reference count first).
template <class T, int32_t LinkBase = 0>
class TSHashTable {
    public:
    TSExplicitList<T> m_fulllist;  // 0x08
    uint32_t m_count = 0;          // 0x20
    // TSFixedArray<TSExplicitList<T, 0xDDDDDDDD, 8>, 8>
    uint32_t m_slotcount = 0;      // 0x28
    uint32_t m_unused2C = 0;
    TSExplicitList<T>* m_slots = nullptr;  // 0x30
    uint32_t m_slotcapacity = 0;   // 0x38
    uint32_t m_unused3C = 0;
    uint32_t m_slotgranularity = 0;  // 0x40
    uint32_t m_unused44 = 0;
    uint32_t m_slotmod = 0xFFFFFFFF;   // 0x48
    uint32_t m_slotlimit = 0x80000000; // 0x4C

    // the static constructor (0x1401cbd00)
    TSHashTable() {
        this->m_fulllist.m_terminator.m_next =
            reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(&this->m_fulllist.m_terminator) | 1);
        this->m_fulllist.UnlinkAll();
        this->m_fulllist.InitEmpty(LinkBase + 0x10);
    }

    // Unlinks every node (without deleting it), then frees the bucket array and empties the
    // full list. Run at exit for static tables (0x1425dba80).
    // ref: FUN_1403236d0
    virtual ~TSHashTable() {
        this->Clear(true);
        // FUN_140323550: TSFixedArray's destructor
        for (uint32_t i = 0; i < this->m_slotcount; i++) {
            this->m_slots[i].UnlinkAll();
            this->m_slots[i].m_terminator.Unlink();
        }
        if (this->m_slots) {
            SMemFree(this->m_slots, TSHashTableTags<T>::s_fixedArray, -2, 0);
        }
        this->m_fulllist.UnlinkAll();
        this->m_fulllist.m_terminator.Unlink();
    }

    virtual void Delete(T* node) = 0;                                      // vtable[0]
    virtual T* NewNode(TSExplicitList<T>* slot, uint32_t extra, uint32_t flags) = 0;  // vtable[1]

    // ref: FUN_140325200
    T* Ptr(const char* key) {
        if (this->m_slotmod == 0xFFFFFFFF) {
            return nullptr;
        }
        uint32_t hash = SStrHashHT(key);
        TSExplicitList<T>* slot = &this->m_slots[hash % this->m_slotmod];
        for (T* node = slot->Head(); node; ) {
            T* next = slot->Link(node)->m_next;
            if (node->m_hashval == hash && (node->m_str == key || _stricmp(node->m_str, key) == 0)) {
                return node;
            }
            uintptr_t raw = reinterpret_cast<uintptr_t>(next);
            node = (raw & 1) || raw == 0 ? nullptr : next;
        }
        return nullptr;
    }

    // The node whose hash is `hashval` and that `match` accepts, or null.
    template <class Match>
    T* Find(uint32_t hashval, Match match) const {
        if (this->m_slotmod == 0xFFFFFFFF) {
            return nullptr;
        }
        TSExplicitList<T>* slot = &this->m_slots[hashval % this->m_slotmod];
        for (T* node = slot->Head(); node; node = slot->Link(node)->Next()) {
            if (node->m_hashval == hashval && match(node)) {
                return node;
            }
        }
        return nullptr;
    }

    // ref: FUN_140324e70
    T* New(uint32_t hashval, uint32_t extra, uint32_t flags) {
        uint32_t index = this->PrepareSlot(hashval);
        T* node = this->NewNode(&this->m_slots[index], extra, flags);
        TSLink<T>* full = node ? reinterpret_cast<TSLink<T>*>(reinterpret_cast<uintptr_t>(node) + this->m_fulllist.m_linkoffset)
                               : &this->m_fulllist.m_terminator;
        full->Unlink();
        TSLink<T>* last = this->m_fulllist.m_terminator.m_prevlink;
        full->m_prevlink = last;
        full->m_next = last->m_next;
        last->m_next = node;
        this->m_fulllist.m_terminator.m_prevlink = full;
        node->m_table = this;
        this->m_count++;
        return node;
    }

    // Links a node the caller allocated: at the tail of its bucket and of the full list
    // (TSHashTable<FACEDATA, HASHKEY_DWORD, 8>::Insert, FUN_140692fb0).
    void Insert(T* node, uint32_t hashval) {
        uint32_t index = this->PrepareSlot(hashval);
        if (node->m_table != nullptr) {
            static_cast<TSHashTable*>(node->m_table)->m_count--;
        }
        this->m_slots[index].LinkToTail(node);
        TSLink<T>* full = reinterpret_cast<TSLink<T>*>(reinterpret_cast<uintptr_t>(node) + this->m_fulllist.m_linkoffset);
        full->Unlink();
        TSLink<T>* last = this->m_fulllist.m_terminator.m_prevlink;
        full->m_prevlink = last;
        full->m_next = last->m_next;
        last->m_next = node;
        this->m_fulllist.m_terminator.m_prevlink = full;
        node->m_table = this;
        this->m_count++;
    }

    // ref: FUN_140325650
    void Unlink(T* node) {
        if (node->m_linktoslot.m_next == nullptr) {
            return;
        }
        node->m_linktoslot.Unlink();
        node->m_linktofull.Unlink();
        node->m_table = nullptr;
        this->m_count--;
    }

    // ref: FUN_140324d50
    void Clear(bool keepNodes) {
        this->m_fulllist.UnlinkAll();
        if (keepNodes) {
            for (uint32_t i = 0; i < this->m_slotcount; i++) {
                this->m_slots[i].UnlinkAll();
            }
        } else {
            for (uint32_t i = 0; i < this->m_slotcount; i++) {
                while (T* node = this->m_slots[i].Head()) {
                    this->Delete(node);
                }
            }
        }
        this->m_count = 0;
    }

    protected:
    // The bucket a hash goes to, creating the buckets on first use and growing them past
    // twice the node count (the head of New and Insert).
    uint32_t PrepareSlot(uint32_t hashval) {
        uint32_t mod = this->m_slotmod;
        if (mod == 0xFFFFFFFF) {
            this->m_slotmod = 3;
            this->ResizeSlots(4);
            for (uint32_t i = 0; i <= this->m_slotmod; i++) {
                TSExplicitList<T>* slot = &this->m_slots[i];
                if (slot->m_linkoffset != LinkBase) {
                    slot->UnlinkAll();
                    slot->InitEmpty(LinkBase);
                }
            }
            mod = this->m_slotmod;
        }

        uint32_t index = hashval % mod;
        if (mod < this->m_count * 2 + 2) {
            uint32_t target = this->m_slotlimit;
            if (mod * 2 + 2 <= target) {
                target = mod * 2 + 1;
                target |= target >> 1;
                target |= target >> 2;
                target |= target >> 4;
                target |= target >> 8;
                target = (target | (target >> 16)) + 1;
            }
            if (mod + 1 < target) {
                this->Rehash(target);
                index = hashval % this->m_slotmod;
            }
        }
        return index;
    }

    // TSGrowableArray<TSExplicitList<T, 0xDDDDDDDD, 8>, 8>::SetCount (granularity 10)
    // ref: FUN_1403254c0
    void ResizeSlots(uint32_t count) {
        if (this->m_slotcount < count) {
            if (this->m_slotcapacity < count) {
                uint32_t granularity = this->m_slotgranularity;
                if (granularity == 0) {
                    if (count < 10) {
                        granularity = count;
                        for (uint32_t low = (count - 1) & count; low != 0; low = (low - 1) & low) {
                            granularity = low;
                        }
                        if (granularity == 0) {
                            granularity = 1;
                        }
                    } else {
                        this->m_slotgranularity = 10;
                        granularity = 10;
                    }
                }
                uint32_t capacity = count;
                if (count % granularity != 0) {
                    capacity = (granularity - count % granularity) + count;
                }
                this->ReallocSlots(capacity);
            }
            for (uint32_t i = this->m_slotcount; i < count; i++) {
                this->m_slots[i].InitEmpty(static_cast<int32_t>(0xDDDDDDDD));
            }
            this->m_slotcount = count;
            return;
        }
        for (uint32_t i = count; i < this->m_slotcount; i++) {
            this->m_slots[i].UnlinkAll();
            this->m_slots[i].m_terminator.Unlink();
        }
        this->m_slotcount = count;
    }

    // TSGrowableArray's reallocation. The new lists are empty (keeping each old list's link
    // offset); nodes are not carried over -- the old lists are emptied and freed. Rehash has
    // already taken every node out before the array grows.
    // ref: FUN_140325310
    void ReallocSlots(uint32_t capacity) {
        const char* s_tag = TSHashTableTags<T>::s_growableArray;
        TSExplicitList<T>* old = this->m_slots;

        for (uint32_t i = capacity; i < this->m_slotcount; i++) {
            old[i].UnlinkAll();
            old[i].m_terminator.Unlink();
        }

        this->m_slotcapacity = capacity;
        this->m_slots = nullptr;
        if (capacity != 0) {
            this->m_slots = static_cast<TSExplicitList<T>*>(SMemAlloc(sizeof(TSExplicitList<T>) * capacity, 8, s_tag, 0, -7, 0));
        }

        if (old) {
            uint32_t keep = this->m_slotcount < capacity ? this->m_slotcount : capacity;
            for (uint32_t i = 0; i < keep; i++) {
                this->m_slots[i].InitEmpty(old[i].m_linkoffset);
                old[i].UnlinkAll();
                old[i].m_terminator.Unlink();
            }
            SMemFree(old, s_tag, -2, 0);
        }
    }

    // Collects every node from the buckets in order, resizes the bucket array, and links
    // each node at the tail of its new bucket. The reference splices a bucket's whole chain
    // onto its temporary list when the link offsets match and moves node by node otherwise
    // (0x140323750); both keep the order this moves them in.
    // ref: FUN_140324a20
    void Rehash(uint32_t count) {
        TSExplicitList<T> moving;
        moving.InitEmpty(LinkBase);
        // modulus + 1 buckets: none on a table never used (the modulus is still -1)
        uint32_t buckets = this->m_slotmod + 1;
        for (uint32_t i = 0; i < buckets; i++) {
            TSExplicitList<T>* slot = &this->m_slots[i];
            while (T* node = slot->Head()) {
                slot->Link(node)->Unlink();
                moving.LinkToTail(node);
            }
        }

        this->ResizeSlots(count);
        for (uint32_t i = 0; i < count; i++) {
            TSExplicitList<T>* slot = &this->m_slots[i];
            if (slot->m_linkoffset != LinkBase) {
                slot->UnlinkAll();
                slot->InitEmpty(LinkBase);
            }
        }
        this->m_slotmod = count - 1;

        while (T* node = moving.Head()) {
            moving.Link(node)->Unlink();
            this->m_slots[node->m_hashval % this->m_slotmod].LinkToTail(node);
        }
        moving.m_terminator.Unlink();
    }
};

#endif
