#include "util/Fnv.hpp"

namespace t2util {

// ref: FUN_140216fc0
uint64_t FnvHashStringI(const char* string, uint64_t hash) {
    for (; *string != '\0'; string++) {
        hash = FnvStepI(*string, hash);
    }
    return hash;
}

}
