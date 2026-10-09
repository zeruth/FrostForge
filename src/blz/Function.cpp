#include "blz/Function.hpp"
#include <cstring>

namespace blz {

void function_empty_manager(void* dest, void* src, int32_t op) {
    (void)dest;
    (void)src;
    (void)op;
}

// ref: FUN_140215350
void function_trivial_manager(void* dest, void* src, int32_t op) {
    if (op == 1 || op == 2) {
        std::memcpy(dest, src, 16);
    } else if (op == 3) {
        *static_cast<void**>(src) = dest;
    }
}

}
