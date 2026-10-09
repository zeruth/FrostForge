#ifndef CASC_HASH_HPP
#define CASC_HASH_HPP

// Bob Jenkins' lookup3 (hashlittle / hashlittle2) as casc uses it for index and segment
// header checksums.

#include <cstdint>

namespace casc {

uint32_t HashLittle(const void* data, uint64_t length, uint32_t initval);
void HashLittle2(const void* data, uint64_t length, uint32_t* pc, uint32_t* pb);

}

#endif
