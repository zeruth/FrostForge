#include "db/GlobalColor.hpp"

#include <cstddef>
#include <cstring>
#include <new>

#include "blz/RbTree.hpp"
#include "blz/String.hpp"
#include "db/DB2Tables.hpp"
#include "db/DynamicLifeTimeDB.hpp"
#include "db/WowDB2Lookup.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

// blz::map<blz::string, ColorInfo>'s node: the tree links, the name, the colour (0x58 bytes).
struct COLORNODE {
    blz::rb_node_base m_links;  // 0x00
    blz::string m_name;         // 0x20
    COLORINFO m_info;           // 0x48
};
static_assert(sizeof(COLORNODE) == 0x58, "the reference allocates 0x58-byte nodes");
static_assert(offsetof(COLORNODE, m_info) == 0x48, "the lookup returns the node + 0x48");

// The map: the tree's header node (parent = root, left = leftmost, right = rightmost), then
// the count.
struct COLORMAP {
    blz::rb_node_base m_header;  // 0x00
    uint64_t m_count;            // 0x20
};

// What a name the table lacks resolves to. The reference's carries a pointer to "ffff00ff" in
// the hex bytes, which nothing reads.
COLORINFO s_missing = { 0xffff00ffu, {} };  // 0x142d1ad40
COLORMAP s_colors = { { nullptr, &s_colors.m_header, &s_colors.m_header, 0 }, 0 };  // 0x142d1ad50
bool s_colorsRead;  // 0x14311bf80
DynamicLifeTimeDB s_globalColor(&g_globalColorDB);  // DAT_14311bf98

const char s_nodeTag[] = ".?AU?$rb_node@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@"
                         "VColorInfo@@@blz@@@blz@@";

// blz::string's ordering: the shared prefix, then the shorter first.
int Compare(const char* a, uint64_t aLength, const char* b, uint64_t bLength) {
    uint64_t n = aLength < bLength ? aLength : bLength;
    int result = n == 0 ? 0 : std::memcmp(a, b, n);
    if (result == 0) {
        result = static_cast<int>(aLength) - static_cast<int>(bLength);
    }
    return result;
}

const blz::string& NameOf(blz::rb_node_base* node) {
    return reinterpret_cast<COLORNODE*>(node)->m_name;
}

// Where a new key would go: (parent, the parent's child slot), or (the equal node, null) when
// the key is already there (blz::map's insert-unique position).
// ref: FUN_14166b780
void InsertPosition(COLORMAP* map, const blz::string& key, blz::rb_node_base** parent, blz::rb_node_base*** slot) {
    blz::rb_node_base* header = &map->m_header;
    blz::rb_node_base* y = header;
    blz::rb_node_base* x = header->m_parent;
    int less = -1;
    while (x != nullptr) {
        y = x;
        less = Compare(key.m_data, key.m_size, NameOf(x).m_data, NameOf(x).m_size);
        x = less < 0 ? x->m_left : x->m_right;
    }
    blz::rb_node_base* j = y;
    if (less < 0) {
        if (y == header->m_left) {
            *parent = y;
            *slot = &y->m_left;
            return;
        }
        j = blz::rb_prev(y);
    }
    if (Compare(NameOf(j).m_data, NameOf(j).m_size, key.m_data, key.m_size) < 0) {
        *parent = y;
        *slot = less < 0 ? &y->m_left : &y->m_right;
        return;
    }
    *parent = j;
    *slot = nullptr;
}

// Reads every GlobalColor row into the map, holding the table while it does.
// ref: FUN_1416728e0
void ReadColors() {
    if (s_colorsRead) {
        return;
    }
    s_globalColor.Acquire();
    s_globalColor.Wait();
    WowDB2Lookup* lookup = g_globalColorDB.m_lookup;
    if (lookup != nullptr) {
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        while (it.m_index != -2) {
            auto record = static_cast<const int32_t*>(it.m_record);
            const char* name = record[0] != 0 ? reinterpret_cast<const char*>(record) + record[0] : "";
            blz::string key;
            key.assign(name);
            COLORINFO info;
            info.m_color = static_cast<uint32_t>(
                DB2FieldI32(reinterpret_cast<const uint8_t*>(record + 1), 1, 4, &g_globalColorDB));
            SStrPrintf(info.m_hex, 9, "%x", info.m_color);

            void* memory = SMemAlloc(sizeof(COLORNODE), 0xb, s_nodeTag, SStrHashFnv1a(s_nodeTag), -6, 0);
            auto node = static_cast<COLORNODE*>(memory);
            new (&node->m_name) blz::string(static_cast<blz::string&&>(key));
            node->m_info = info;

            blz::rb_node_base* parent;
            blz::rb_node_base** slot;
            InsertPosition(&s_colors, node->m_name, &parent, &slot);
            if (slot == nullptr) {
                // a name the table repeats: the first row's colour stands
                node->m_name.~string();
                SMemFree(node, s_nodeTag, -6, 0);
            } else {
                blz::rb_node_base* header = &s_colors.m_header;
                s_colors.m_count++;
                node->m_links.m_parent = parent;
                node->m_links.m_left = nullptr;
                node->m_links.m_right = nullptr;
                node->m_links.m_color = 0;
                if (parent == header) {
                    header->m_parent = &node->m_links;
                    header->m_left = &node->m_links;
                    header->m_right = &node->m_links;
                } else {
                    *slot = &node->m_links;
                    if (parent == header->m_left && slot == &parent->m_left) {
                        header->m_left = &node->m_links;
                    }
                    if (parent == header->m_right && slot == &parent->m_right) {
                        header->m_right = &node->m_links;
                    }
                }
                blz::rb_insert_fixup(&header->m_parent, &node->m_links);
            }
            lookup->Next(&it);
        }
    }
    s_globalColor.Release();
    s_colorsRead = true;
}

}  // namespace

// ref: FUN_141672440
const COLORINFO* GlobalColorFind(const char* name, bool* found) {
    bool ignored;
    if (found == nullptr) {
        found = &ignored;
    }
    if (name == nullptr) {
        *found = false;
        return nullptr;
    }
    if (!s_colorsRead) {
        ReadColors();
    }
    uint64_t length = std::strlen(name);
    blz::rb_node_base* header = &s_colors.m_header;
    blz::rb_node_base* y = header;
    for (blz::rb_node_base* x = header->m_parent; x != nullptr;) {
        if (Compare(NameOf(x).m_data, NameOf(x).m_size, name, length) < 0) {
            x = x->m_right;
        } else {
            y = x;
            x = x->m_left;
        }
    }
    if (y != header && Compare(NameOf(y).m_data, NameOf(y).m_size, name, length) <= 0) {
        *found = true;
        return &reinterpret_cast<COLORNODE*>(y)->m_info;
    }
    *found = false;
    return nullptr;
}

// ref: FUN_141672420
const uint8_t* GlobalColorLookup(const char* name, bool* found) {
    const COLORINFO* info = GlobalColorFind(name, found);
    return reinterpret_cast<const uint8_t*>(info != nullptr ? info : &s_missing);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14166b780 = (void*)&InsertPosition;
extern "C" void* const gen_f_1416728e0 = (void*)&ReadColors;
extern "C" void* const gen_p_142d1ad40 = (void*)&s_missing;
// ---- end of generated globals ----
