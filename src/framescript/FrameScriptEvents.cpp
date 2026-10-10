#include "framescript/FrameScriptEvents.hpp"

#include <algorithm>

#include "blz/Murmur.hpp"
#include "framescript/EventIds.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace FrameScriptEvents {

const char s_listenerTag[] = ".?AUEventListener@FrameScriptEvents@@";
const char s_deferredTag[] = ".?AUDeferredRegistration@FrameScriptEvents@@";
const char s_objectTag[] = ".PEAVFrameScript_Object@@";

namespace {

const char s_nodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBW4EventID@LanguageBinding@@VEventListenerList@"
                         "FrameScriptEvents@@@blz@@@blz@@";
const char s_bucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBW4EventID@LanguageBinding@@VEventListenerList@"
                           "FrameScriptEvents@@@blz@@@blz@@";

// The units a registration without a filter carries (0x1426cd678).
constexpr uint64_t NO_UNITS = 0;

// A node of the event map (0x30): blz::chained_hash_node<pair<EventID const, EventListenerList>>.
struct EventNode {
    EventNode* m_next;
    uint64_t m_key;
    EventListenerList m_list;
};
static_assert(sizeof(EventNode) == 0x30, "the event map node is 0x30 bytes");

// The event map (0x142cf5788): a chained table whose buckets are allocated on first use.
struct EventMap {
    uint64_t m_bucketCount = 8;
    EventNode** m_buckets = nullptr;
    uint64_t m_size = 0;
    float m_maxLoad = 1.0f;
};

EventMap s_events;

// The listeners of every event (0x142e9a080).
EventListenerList s_allEvents;

uint64_t Bucket(uint64_t key, uint64_t count) {
    return blz::fmix64(key) & (count - 1);
}

// ref: FUN_140648c20
void Rehash(EventMap* map, uint64_t count) {
    float needed = static_cast<float>(map->m_size) / map->m_maxLoad;
    uint64_t wanted = static_cast<uint64_t>(needed);
    if (static_cast<float>(map->m_size) > static_cast<float>(wanted) * map->m_maxLoad) {
        wanted++;
    }
    if (wanted < count) {
        wanted = count;
    }
    uint64_t buckets = 8;
    while (buckets < wanted) {
        buckets *= 2;
    }
    if (buckets == map->m_bucketCount && map->m_buckets != nullptr) {
        return;
    }
    auto table = static_cast<EventNode**>(SMemAlloc(buckets * 8, 0xb, s_bucketTag, SStrHashFnv1a(s_bucketTag), -6, 0));
    std::memset(table, 0, buckets * 8);
    if (map->m_buckets != nullptr) {
        for (uint64_t i = 0; i < map->m_bucketCount; i++) {
            EventNode* node = map->m_buckets[i];
            while (node != nullptr) {
                EventNode* next = node->m_next;
                uint64_t index = Bucket(node->m_key, buckets);
                node->m_next = table[index];
                table[index] = node;
                node = next;
            }
        }
    }
    SMemFree(map->m_buckets, s_bucketTag, -6, 0);
    map->m_buckets = table;
    map->m_bucketCount = buckets;
}

EventListenerList* Find(uint64_t id) {
    if (s_events.m_buckets == nullptr) {
        return nullptr;
    }
    for (EventNode* node = s_events.m_buckets[Bucket(id, s_events.m_bucketCount)]; node != nullptr; node = node->m_next) {
        if (node->m_key == id) {
            return &node->m_list;
        }
    }
    return nullptr;
}

DeferredRegistration* FindDeferred(DeferredList* deferred, FrameScript_Object* object) {
    for (DeferredRegistration& entry : *deferred) {
        if (entry.m_object == object) {
            return &entry;
        }
    }
    return nullptr;
}

// Swap-removes a listener; false if the object is not in the list.
bool RemoveListener(EventListenerList* list, FrameScript_Object* object) {
    for (uint64_t i = 0; i < list->m_listeners.size(); i++) {
        if (list->m_listeners[i].m_object == object) {
            list->m_listeners.erase_unordered(i);
            return true;
        }
    }
    return false;
}

}

// ref: FUN_140647f40
bool EventListenerList::Defer(FrameScript_Object* object, uint64_t units, int32_t op) {
    DeferredList* deferred = this->m_deferred;
    for (uint64_t i = 0; i < deferred->size(); i++) {
        DeferredRegistration& entry = (*deferred)[i];
        if (entry.m_object == object) {
            if (entry.m_op == op) {
                return false;
            }
            // the opposite operation cancels the queued one
            deferred->erase_unordered(i);
            return true;
        }
    }
    // FUN_1406472e0
    deferred->push_back({object, units, op});
    return true;
}

// ref: FUN_140647fc0
int32_t EventListenerList::Add(FrameScript_Object* object, uint64_t units) {
    if (this->m_deferred != nullptr) {
        return this->Defer(object, units, 0) ? 0 : 1;
    }
    for (EventListener& listener : this->m_listeners) {
        if (listener.m_object == object) {
            uint16_t wanted[4];
            std::memcpy(wanted, &units, sizeof(wanted));
            for (uint32_t i = 0; i < 4; i++) {
                if (wanted[i] != listener.m_units[i]) {
                    std::memcpy(listener.m_units, &units, sizeof(listener.m_units));
                    return 0;
                }
            }
            return 1;
        }
    }
    EventListener added;
    added.m_object = object;
    std::memcpy(added.m_units, &units, sizeof(added.m_units));
    this->m_listeners.push_back(added);
    return 0;
}

// ref: FUN_140648590
int32_t EventListenerList::Remove(FrameScript_Object* object) {
    if (this->m_deferred != nullptr) {
        return this->Defer(object, NO_UNITS, 1) ? 0 : 1;
    }
    return RemoveListener(this, object) ? 0 : 1;
}

// ref: FUN_1406479f0
blz::vector<FrameScript_Object*, s_objectTag>* EventListenerList::Collect(blz::vector<FrameScript_Object*, s_objectTag>* out) {
    uint64_t added = 0;
    if (this->m_deferred != nullptr) {
        for (DeferredRegistration& entry : *this->m_deferred) {
            if (entry.m_op == 0) {
                added++;
            }
        }
    }
    // FUN_140648e50
    out->reserve(this->m_listeners.size() + added);
    for (EventListener& listener : this->m_listeners) {
        out->push_back(listener.m_object);
    }
    if (this->m_deferred != nullptr) {
        for (DeferredRegistration& entry : *this->m_deferred) {
            if (entry.m_op == 0) {
                out->push_back(entry.m_object);
            }
        }
    }
    return out;
}

EventListenerList* FindEventList(uint64_t id) {
    if (s_events.m_buckets == nullptr) {
        return nullptr;
    }
    for (EventNode* node = s_events.m_buckets[Bucket(id, s_events.m_bucketCount)]; node != nullptr; node = node->m_next) {
        if (node->m_key == id) {
            return &node->m_list;
        }
    }
    return nullptr;
}

// ref: FUN_140648490
bool ShouldDeliver(EventListenerList* list, EventListener* listener) {
    DeferredList* deferred = list->m_deferred;
    for (DeferredRegistration& entry : *deferred) {
        if (entry.m_object == listener->m_object) {
            if (entry.m_op == 1) {
                return false;
            }
            break;
        }
    }
    return listener->m_object->HasScriptAny(0x1a);
}

// ref: FUN_140647480
DispatchScope::DispatchScope(EventListenerList* list) : m_list(list) {
    if (list->m_deferred == nullptr) {
        list->m_deferred = &this->m_deferred;
    }
}

// ref: FUN_140647580
DispatchScope::~DispatchScope() {
    EventListenerList* list = this->m_list;
    if (list->m_deferred != &this->m_deferred) {
        return;
    }
    list->m_deferred = nullptr;
    for (DeferredRegistration& entry : this->m_deferred) {
        if (entry.m_op == 0) {
            list->Add(entry.m_object, entry.m_units);
        } else if (entry.m_op == 1) {
            if (list->m_deferred == nullptr) {
                RemoveListener(list, entry.m_object);
            } else {
                list->Defer(entry.m_object, NO_UNITS, 1);
            }
        }
    }
}

// The upper-cased name, hashed.
// ref: FUN_1407bc5a0
uint64_t GetEventID(const char* name, uint64_t length) {
    char* upper = static_cast<char*>(_alloca((length + 0xf) & ~0xfull));
    for (uint64_t i = 0; i < length; i++) {
        char c = name[i];
        upper[i] = static_cast<uint8_t>(c - 'a') < 26 ? static_cast<char>(c & 0xdf) : c;
    }
    uint64_t hash[2];
    blz::murmur3_128(hash, upper, length, 0);
    return hash[0];
}

// A compiled search over the ids of every event the client defines.
// ref: FUN_140af6990
bool IsKnownEvent(uint64_t id) {
    return std::binary_search(s_eventIds, s_eventIds + EVENT_ID_COUNT, id);
}

// ref: FUN_140647680
EventListenerList* GetEventList(uint64_t id) {
    if (s_events.m_buckets == nullptr) {
        Rehash(&s_events, s_events.m_bucketCount);
    }
    EventNode** head = &s_events.m_buckets[Bucket(id, s_events.m_bucketCount)];
    for (EventNode* node = *head; node != nullptr; node = node->m_next) {
        if (node->m_key == id) {
            return &node->m_list;
        }
    }
    auto node = static_cast<EventNode*>(SMemAlloc(sizeof(EventNode), 0xb, s_nodeTag, SStrHashFnv1a(s_nodeTag), -6, 0));
    node->m_next = *head;
    node->m_key = id;
    new (&node->m_list) EventListenerList();
    *head = node;
    s_events.m_size++;
    float load = static_cast<float>(s_events.m_size) / static_cast<float>(s_events.m_bucketCount);
    if (s_events.m_maxLoad <= load && load != s_events.m_maxLoad) {
        Rehash(&s_events, s_events.m_bucketCount * 2);
    }
    return &node->m_list;
}

EventListenerList* GetAllEventsList() {
    return &s_allEvents;
}

// ref: FUN_140648220
int32_t RegisterEvent(FrameScript_Object* object, uint64_t id) {
    if (!IsKnownEvent(id) && !IsKnownEvent(id) && !IsKnownEvent(id)) {
        return 2;
    }
    return GetEventList(id)->Add(object, NO_UNITS);
}

// ref: FUN_1406488a0
int32_t UnregisterEvent(FrameScript_Object* object, uint64_t id) {
    if (!IsKnownEvent(id) && !IsKnownEvent(id) && !IsKnownEvent(id)) {
        return 2;
    }
    EventListenerList* list = Find(id);
    if (list == nullptr) {
        return 1;
    }
    return list->Remove(object);
}

// ref: FUN_140648200
void RegisterAllEvents(FrameScript_Object* object) {
    s_allEvents.Add(object, NO_UNITS);
}

// ref: FUN_140648610
int32_t UnregisterAllEvents(FrameScript_Object* object) {
    int32_t result = 0;
    if (s_allEvents.m_deferred == nullptr) {
        if (!RemoveListener(&s_allEvents, object)) {
            result = 1;
        }
    } else {
        DeferredRegistration* entry = FindDeferred(s_allEvents.m_deferred, object);
        if (entry == nullptr) {
            s_allEvents.m_deferred->push_back({object, NO_UNITS, 1});
        } else if (entry->m_op == 1) {
            result = 1;
        } else {
            s_allEvents.m_deferred->erase_unordered(entry - s_allEvents.m_deferred->begin());
        }
    }
    bool removed = false;
    if (s_events.m_buckets != nullptr) {
        for (uint64_t i = 0; i < s_events.m_bucketCount; i++) {
            for (EventNode* node = s_events.m_buckets[i]; node != nullptr; node = node->m_next) {
                EventListenerList* list = &node->m_list;
                if (list->m_deferred == nullptr) {
                    if (RemoveListener(list, object)) {
                        removed = true;
                    }
                } else if (list->Defer(object, NO_UNITS, 1)) {
                    removed = true;
                }
            }
        }
    }
    return removed ? 0 : result;
}

// ref: FUN_140647e50
bool IsEventRegistered(FrameScript_Object* object, uint64_t id) {
    EventListenerList* list = Find(id);
    if (list == nullptr) {
        return false;
    }
    for (EventListener& listener : list->m_listeners) {
        if (listener.m_object == object) {
            return true;
        }
    }
    if (list->m_deferred != nullptr) {
        DeferredRegistration* entry = FindDeferred(list->m_deferred, object);
        if (entry != nullptr) {
            return entry->m_op == 0;
        }
    }
    return false;
}

// ref: FUN_140647d00
bool GetEventUnits(FrameScript_Object* object, uint64_t id, uint64_t* units) {
    EventListenerList* list = Find(id);
    if (list == nullptr) {
        return false;
    }
    if (list->m_deferred != nullptr) {
        DeferredRegistration* entry = FindDeferred(list->m_deferred, object);
        if (entry != nullptr) {
            if (entry->m_op != 0) {
                return false;
            }
            *units = entry->m_units;
            return true;
        }
    }
    for (EventListener& listener : list->m_listeners) {
        if (listener.m_object == object) {
            std::memcpy(units, listener.m_units, sizeof(*units));
            return true;
        }
    }
    return false;
}

// ref: FUN_140647b70
blz::vector<FrameScript_Object*, s_objectTag>* GetListeners(blz::vector<FrameScript_Object*, s_objectTag>* out, uint64_t id) {
    s_allEvents.Collect(out);
    EventListenerList* list = Find(id);
    if (list != nullptr) {
        blz::embedded_vector<FrameScript_Object*, 32, s_objectTag> listeners;
        list->Collect(&listeners);
        // FUN_140648a50: inserted at the end
        out->reserve(out->size() + listeners.size());
        for (FrameScript_Object* object : listeners) {
            out->push_back(object);
        }
    }
    return out;
}

// ref: FUN_140647840
void Clear() {
    if (s_events.m_buckets != nullptr) {
        for (uint64_t i = 0; i < s_events.m_bucketCount; i++) {
            EventNode* node = s_events.m_buckets[i];
            while (node != nullptr) {
                EventNode* next = node->m_next;
                node->m_list.~EventListenerList();
                SMemFree(node, s_nodeTag, -6, 0);
                node = next;
            }
            s_events.m_buckets[i] = nullptr;
        }
    }
    s_events.m_size = 0;
    s_allEvents.m_listeners.m_size = 0;
}

}
