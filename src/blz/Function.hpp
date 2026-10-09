#ifndef BLZ_FUNCTION_HPP
#define BLZ_FUNCTION_HPP

#include <cstdint>

namespace blz {

// The type-erased callable the 3.4.3 engine stores handlers in (blz::function): an invoker,
// a manager, and 16 bytes of inline storage. The manager is called as
// manager(dest, src, op): op 0 destroys dest, 1 copies src into dest, 2 moves it, 3 writes the
// address of the stored callable through src. (A heap-held callable shows the difference: op 1
// allocates a new block, op 2 takes the pointer, 0x1403094d0.) An empty function's manager is a
// function that does nothing.
struct function_storage {
    void* m_invoke;
    void (*m_manager)(void* dest, void* src, int32_t op);
    alignas(8) uint8_t m_storage[16];
};
static_assert(sizeof(function_storage) == 0x20, "blz::function is 0x20 bytes");

// The empty manager. In the reference this is _guard_check_icall (a bare `ret` that every
// empty function shares through identical-code folding).
void function_empty_manager(void* dest, void* src, int32_t op);

// The manager for a callable that fits the storage and needs no destruction (a function
// pointer): copy and move are a 16-byte copy, destroy does nothing.
void function_trivial_manager(void* dest, void* src, int32_t op);

}

#endif
