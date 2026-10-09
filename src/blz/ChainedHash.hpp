#ifndef BLZ_CHAINED_HASH_HPP
#define BLZ_CHAINED_HASH_HPP

// blz's chained hash table (unordered_map / unordered_set over chained_hash_node) as the UI
// root's maps lay it out (0x20): the bucket count (a power of two, 8 at least), the buckets
// (allocated on first insert), the size and the load limit. Keys are pointers, hashed with
// fmix64. A node starts with the next link and the key; a map's value follows.
//
// The operations are the ones the reference instantiates per map (CSimpleTop's frame batches
// 0x1405ee830/0x1405ee300, animating objects 0x1405eb740/0x1405f3120, the rehash 0x1405f4fe0).

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "blz/Murmur.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace blz {

struct chained_hash_header {
    uint64_t m_bucketCount = 8;
    void** m_buckets = nullptr;
    uint64_t m_size = 0;
    float m_maxLoad = 1.0f;
};
static_assert(sizeof(chained_hash_header) == 0x20, "a chained hash table is 0x20 bytes");

inline uint64_t chained_bucket(uint64_t key, uint64_t count) {
    return fmix64(key) & (count - 1);
}

// A key's bits: pointers by address, integers as they are.
template <class Key>
uint64_t chained_key_bits(Key key) {
    if constexpr (std::is_pointer_v<Key>) {
        return chained_key_bits(key);
    } else {
        return static_cast<uint64_t>(key);
    }
}

// Resizes to the smallest power of two (8 at least) that holds the table at its load limit
// and is at least `count`; nothing happens when that is the current size.
template <class Node>
void chained_rehash(chained_hash_header* table, uint64_t count, const char* bucketTag) {
    float needed = static_cast<float>(table->m_size) / table->m_maxLoad;
    uint64_t wanted = static_cast<uint64_t>(needed);
    if (static_cast<float>(table->m_size) > static_cast<float>(wanted) * table->m_maxLoad) {
        wanted++;
    }
    if (wanted < count) {
        wanted = count;
    }
    uint64_t buckets = 8;
    while (buckets < wanted) {
        buckets *= 2;
    }
    if (buckets == table->m_bucketCount && table->m_buckets != nullptr) {
        return;
    }
    auto fresh = static_cast<void**>(SMemAlloc(buckets * 8, 0xb, bucketTag, SStrHashFnv1a(bucketTag), -6, 0));
    std::memset(fresh, 0, buckets * 8);
    if (table->m_buckets != nullptr) {
        for (uint64_t i = 0; i < table->m_bucketCount; i++) {
            auto node = static_cast<Node*>(table->m_buckets[i]);
            while (node != nullptr) {
                Node* next = node->m_next;
                uint64_t index = chained_bucket(chained_key_bits(node->m_key), buckets);
                node->m_next = static_cast<Node*>(fresh[index]);
                fresh[index] = node;
                node = next;
            }
        }
    }
    SMemFree(table->m_buckets, bucketTag, -6, 0);
    table->m_buckets = fresh;
    table->m_bucketCount = buckets;
}

template <class Node, class Key>
Node* chained_find(chained_hash_header* table, Key key) {
    if (table->m_buckets == nullptr) {
        return nullptr;
    }
    uint64_t index = chained_bucket(chained_key_bits(key), table->m_bucketCount);
    for (auto node = static_cast<Node*>(table->m_buckets[index]); node != nullptr; node = node->m_next) {
        if (node->m_key == key) {
            return node;
        }
    }
    return nullptr;
}

// The node of `key`, made (value-initialised past the key) and linked at the end of its
// bucket when missing; *inserted says which. The table doubles once it passes its load limit.
template <class Node, class Key>
Node* chained_insert(chained_hash_header* table, Key key, bool* inserted, const char* nodeTag, const char* bucketTag) {
    if (table->m_buckets == nullptr) {
        chained_rehash<Node>(table, table->m_bucketCount, bucketTag);
    }
    uint64_t index = chained_bucket(chained_key_bits(key), table->m_bucketCount);
    auto link = reinterpret_cast<Node**>(&table->m_buckets[index]);
    while (*link != nullptr) {
        if ((*link)->m_key == key) {
            *inserted = false;
            return *link;
        }
        link = &(*link)->m_next;
    }
    auto node = static_cast<Node*>(SMemAlloc(sizeof(Node), 0xb, nodeTag, SStrHashFnv1a(nodeTag), -6, 0));
    new (node) Node();
    node->m_next = nullptr;
    node->m_key = key;
    *link = node;
    table->m_size++;
    float load = static_cast<float>(table->m_size) / static_cast<float>(table->m_bucketCount);
    if (table->m_maxLoad < load) {
        chained_rehash<Node>(table, table->m_bucketCount * 2, bucketTag);
    }
    *inserted = true;
    return node;
}

// Unlinks the node of `key` and returns it (the caller frees it), or null.
template <class Node, class Key>
Node* chained_unlink(chained_hash_header* table, Key key) {
    if (table->m_buckets == nullptr) {
        return nullptr;
    }
    uint64_t index = chained_bucket(chained_key_bits(key), table->m_bucketCount);
    auto link = reinterpret_cast<Node**>(&table->m_buckets[index]);
    while (*link != nullptr && (*link)->m_key != key) {
        link = &(*link)->m_next;
    }
    Node* node = *link;
    if (node == nullptr) {
        return nullptr;
    }
    *link = node->m_next;
    table->m_size--;
    return node;
}

template <class Node>
void chained_free_node(Node* node, const char* nodeTag) {
    node->~Node();
    SMemFree(node, nodeTag, -6, 0);
}

// Calls `visit` on every node, bucket by bucket; a node may not be removed while visited.
template <class Node, class Visit>
void chained_for_each(const chained_hash_header* table, Visit visit) {
    if (table->m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < table->m_bucketCount; i++) {
        for (auto node = static_cast<Node*>(table->m_buckets[i]); node != nullptr; node = node->m_next) {
            visit(node);
        }
    }
}

// Frees every node and the buckets: what the table's destructor does.
template <class Node>
void chained_destroy(chained_hash_header* table, const char* nodeTag, const char* bucketTag) {
    if (table->m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < table->m_bucketCount; i++) {
        auto node = static_cast<Node*>(table->m_buckets[i]);
        while (node != nullptr) {
            Node* next = node->m_next;
            chained_free_node(node, nodeTag);
            node = next;
        }
    }
    SMemFree(table->m_buckets, bucketTag, -6, 0);
    table->m_buckets = nullptr;
    table->m_size = 0;
}

}

#endif
