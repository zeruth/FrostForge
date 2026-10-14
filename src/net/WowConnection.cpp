#include "net/WowConnection.hpp"
#include <cstdint>
#include <windows.h>

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1414fd750(unsigned long long, unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
unsigned long long FUN_141500d00(unsigned long long, unsigned long long);
unsigned long long FUN_1414fe7c0(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_1414fecf0(unsigned long long);
unsigned long long FUN_141500de0(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_1414fed80(unsigned long long);
unsigned long long FUN_141500b90(unsigned long long);
unsigned long long FUN_1415611a0(unsigned long long, unsigned long long);
}

// translated: FUN_1414fd750
WowConnection* WowConnectionConstruct(void* memory, void* response, PacketPipe* pipe, uint64_t a4, uint64_t a5) {
    __try {
        return reinterpret_cast<WowConnection*>(FUN_1414fd750(reinterpret_cast<uintptr_t>(memory),
            reinterpret_cast<uintptr_t>(response), reinterpret_cast<uintptr_t>(pipe), a4, a5));
    } __except (GenFault(0x1414fd750ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// translated: FUN_141500d00
void WowConnectionSetPipe(WowConnection* connection, PacketPipe* pipe) {
    __try {
        FUN_141500d00(reinterpret_cast<uintptr_t>(connection), reinterpret_cast<uintptr_t>(pipe));
    } __except (GenFault(0x141500d00ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1414fe7c0
bool WowConnectionConnect(WowConnection* connection, const OSNETADDRESS* address, uint16_t port, int32_t timeout) {
    __try {
        return (FUN_1414fe7c0(reinterpret_cast<uintptr_t>(connection), reinterpret_cast<uintptr_t>(address), port,
                    static_cast<uint32_t>(timeout)) & 0xff) != 0;
    } __except (GenFault(0x1414fe7c0ull, GetExceptionInformation())) {
        return false;
    }
}

// translated: FUN_1414fecf0
void WowConnectionDisconnect(WowConnection* connection) {
    __try {
        FUN_1414fecf0(reinterpret_cast<uintptr_t>(connection));
    } __except (GenFault(0x1414fecf0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141500de0
void WowConnectionSetResponse(WowConnection* connection, void* response, bool a3) {
    __try {
        FUN_141500de0(reinterpret_cast<uintptr_t>(connection), reinterpret_cast<uintptr_t>(response), a3);
    } __except (GenFault(0x141500de0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1414fed80
void WowConnectionClose(WowConnection* connection) {
    __try {
        FUN_1414fed80(reinterpret_cast<uintptr_t>(connection));
    } __except (GenFault(0x1414fed80ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141500b90
void WowConnectionRelease(WowConnection* connection) {
    __try {
        FUN_141500b90(reinterpret_cast<uintptr_t>(connection));
    } __except (GenFault(0x141500b90ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1415611a0
PacketPipe* PacketPipeConstruct(void* memory, bool a2) {
    __try {
        return reinterpret_cast<PacketPipe*>(FUN_1415611a0(reinterpret_cast<uintptr_t>(memory), a2));
    } __except (GenFault(0x1415611a0ull, GetExceptionInformation())) {
        return nullptr;
    }
}
