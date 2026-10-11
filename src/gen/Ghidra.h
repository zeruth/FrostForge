/*
** The types and helpers Ghidra's decompiler output is written in, so tools/translate.py can
** compile it as C. The generated files (src/gen/translated_*.c) are the reference's functions
** translated from build/corpus.jsonl; they run against the reference's own data sections,
** mapped at their original addresses (src/gen/RefImage.cpp), so a global is its address.
*/
#ifndef GEN_GHIDRA_H
#define GEN_GHIDRA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef unsigned char undefined;
typedef unsigned char undefined1;
typedef unsigned short undefined2;
typedef unsigned int undefined3;
typedef unsigned int undefined4;
typedef unsigned long long undefined5;
typedef unsigned long long undefined6;
typedef unsigned long long undefined7;
typedef unsigned long long undefined8;
typedef unsigned char byte;
typedef unsigned char uchar;
typedef unsigned char uint1;
typedef unsigned short ushort;
typedef unsigned short word;
typedef unsigned short wchar16;
typedef unsigned int uint;
typedef unsigned int uint3;
typedef unsigned int dword;
typedef unsigned long ulong;
typedef long long longlong;
typedef unsigned long long ulonglong;
typedef unsigned long long qword;
typedef unsigned long long uint8;
typedef unsigned int uint4;
typedef unsigned long long uint5;
typedef unsigned long long uint6;
typedef unsigned long long uint7;
typedef int int3;
typedef long long int5;
typedef long long int6;
typedef long long int7;
typedef signed char int1;
typedef unsigned short uint2;
typedef int int4;
typedef long long int8;
typedef short int2;
typedef signed char sbyte;
typedef float float4;
typedef double float8;
typedef long double float10;
typedef unsigned long long ulonglong8;
typedef void *pointer;
typedef void *pointer64;
typedef void *HANDLE;
/* a call through `code *`: any arguments, the result in rax */
typedef unsigned long long code();

/* a reference global: its address in the mapped reference image */
#define REFG(type, addr) (*(type *)(uintptr_t)(addr##ULL))
#define REFP(addr) ((void *)(uintptr_t)(addr##ULL))
/* a call to a translated function, unprototyped: Ghidra's argument counts disagree between a
   function and its callers, and a definition earlier in the same file would otherwise impose its
   own (floats are promoted to double; the definitions take them that way) */
#define CALLF(type, f) ((type (*)())(f))

/* Ghidra's piecewise helpers */
#define CONCAT11(a, b) ((uint16_t)(((uint16_t)(uint8_t)(a) << 8) | (uint8_t)(b)))
#define CONCAT12(a, b) ((uint32_t)(((uint32_t)(uint8_t)(a) << 16) | (uint16_t)(b)))
#define CONCAT13(a, b) ((uint32_t)(((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(b) & 0xffffff)))
#define CONCAT14(a, b) ((uint64_t)(((uint64_t)(uint8_t)(a) << 32) | (uint32_t)(b)))
#define CONCAT15(a, b) ((uint64_t)(((uint64_t)(uint8_t)(a) << 40) | ((uint64_t)(b) & 0xffffffffffULL)))
#define CONCAT16(a, b) ((uint64_t)(((uint64_t)(uint8_t)(a) << 48) | ((uint64_t)(b) & 0xffffffffffffULL)))
#define CONCAT17(a, b) ((uint64_t)(((uint64_t)(uint8_t)(a) << 56) | ((uint64_t)(b) & 0xffffffffffffffULL)))
#define CONCAT21(a, b) ((uint32_t)(((uint32_t)(uint16_t)(a) << 8) | (uint8_t)(b)))
#define CONCAT22(a, b) ((uint32_t)(((uint32_t)(uint16_t)(a) << 16) | (uint16_t)(b)))
#define CONCAT24(a, b) ((uint64_t)(((uint64_t)(uint16_t)(a) << 32) | (uint32_t)(b)))
#define CONCAT26(a, b) ((uint64_t)(((uint64_t)(uint16_t)(a) << 48) | ((uint64_t)(b) & 0xffffffffffffULL)))
#define CONCAT31(a, b) ((uint32_t)((((uint32_t)(a) & 0xffffff) << 8) | (uint8_t)(b)))
#define CONCAT32(a, b) ((uint64_t)((((uint64_t)(a) & 0xffffff) << 16) | (uint16_t)(b)))
#define CONCAT35(a, b) ((uint64_t)((((uint64_t)(a) & 0xffffff) << 40) | ((uint64_t)(b) & 0xffffffffffULL)))
#define CONCAT41(a, b) ((uint64_t)(((uint64_t)(uint32_t)(a) << 8) | (uint8_t)(b)))
#define CONCAT42(a, b) ((uint64_t)(((uint64_t)(uint32_t)(a) << 16) | (uint16_t)(b)))
#define CONCAT44(a, b) ((uint64_t)(((uint64_t)(uint32_t)(a) << 32) | (uint32_t)(b)))
#define CONCAT53(a, b) ((uint64_t)((((uint64_t)(a) & 0xffffffffffULL) << 24) | ((uint32_t)(b) & 0xffffff)))
#define CONCAT62(a, b) ((uint64_t)((((uint64_t)(a) & 0xffffffffffffULL) << 16) | (uint16_t)(b)))
#define CONCAT71(a, b) ((uint64_t)((((uint64_t)(a) & 0xffffffffffffffULL) << 8) | (uint8_t)(b)))
#define SUB41(x, n) ((uint8_t)((uint32_t)(x) >> ((n) * 8)))
#define SUB42(x, n) ((uint16_t)((uint32_t)(x) >> ((n) * 8)))
#define SUB81(x, n) ((uint8_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB82(x, n) ((uint16_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB84(x, n) ((uint32_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB21(x, n) ((uint8_t)((uint16_t)(x) >> ((n) * 8)))
#define ZEXT14(x) ((uint32_t)(uint8_t)(x))
#define ZEXT18(x) ((uint64_t)(uint8_t)(x))
#define ZEXT24(x) ((uint32_t)(uint16_t)(x))
#define ZEXT28(x) ((uint64_t)(uint16_t)(x))
#define ZEXT48(x) ((uint64_t)(uint32_t)(x))
#define ZEXT816(x) ((uint64_t)(x))
#define SEXT14(x) ((int32_t)(int8_t)(x))
#define SEXT18(x) ((int64_t)(int8_t)(x))
#define SEXT24(x) ((int32_t)(int16_t)(x))
#define SEXT28(x) ((int64_t)(int16_t)(x))
#define SEXT48(x) ((int64_t)(int32_t)(x))
#define SEXT816(x) ((int64_t)(x))
#define CARRY1(a, b) ((uint8_t)((uint8_t)(a) + (uint8_t)(b)) < (uint8_t)(a))
#define CARRY2(a, b) ((uint16_t)((uint16_t)(a) + (uint16_t)(b)) < (uint16_t)(a))
#define CARRY4(a, b) ((uint32_t)((uint32_t)(a) + (uint32_t)(b)) < (uint32_t)(a))
#define CARRY8(a, b) ((uint64_t)((uint64_t)(a) + (uint64_t)(b)) < (uint64_t)(a))
#define SCARRY4(a, b) ((((int32_t)(a) ^ (int32_t)((uint32_t)(a) + (uint32_t)(b))) & ((int32_t)(b) ^ (int32_t)((uint32_t)(a) + (uint32_t)(b)))) < 0)
#define SCARRY8(a, b) ((((int64_t)(a) ^ (int64_t)((uint64_t)(a) + (uint64_t)(b))) & ((int64_t)(b) ^ (int64_t)((uint64_t)(a) + (uint64_t)(b)))) < 0)
#define SBORROW4(a, b) ((((int32_t)(a) ^ (int32_t)(b)) & ((int32_t)(a) ^ (int32_t)((uint32_t)(a) - (uint32_t)(b)))) < 0)
#define SBORROW8(a, b) ((((int64_t)(a) ^ (int64_t)(b)) & ((int64_t)(a) ^ (int64_t)((uint64_t)(a) - (uint64_t)(b)))) < 0)
#define POPCOUNT(x) ((int)__popcnt64((uint64_t)(x)))
#define NAN_FLOAT(x) ((x) != (x))
#undef NAN
#define NAN(x) ((x) != (x))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define SQRT(x) sqrt(x)

/* a `lock`-prefixed instruction (Ghidra's LOCK(); ... UNLOCK();), by the size of the lvalue it
   works on; each yields the value before the operation, as the instruction leaves it */
#include <intrin.h>
#define GEN_ATOMIC_(op, lv, v) \
    (sizeof(lv) == 8 ? (long long)_Interlocked##op##64((volatile long long *)&(lv), (long long)(v)) \
     : sizeof(lv) == 4 ? (long long)_Interlocked##op((volatile long *)&(lv), (long)(v)) \
     : sizeof(lv) == 2 ? (long long)_Interlocked##op##16((volatile short *)&(lv), (short)(v)) \
     : (long long)_Interlocked##op##8((volatile char *)&(lv), (char)(v)))
#define GEN_ATOMIC_ADD(lv, v) GEN_ATOMIC_(ExchangeAdd, lv, v)
#define GEN_ATOMIC_AND(lv, v) GEN_ATOMIC_(And, lv, v)
#define GEN_ATOMIC_OR(lv, v) GEN_ATOMIC_(Or, lv, v)
#define GEN_ATOMIC_XOR(lv, v) GEN_ATOMIC_(Xor, lv, v)
#define GEN_ATOMIC_XCHG(lv, v) GEN_ATOMIC_(Exchange, lv, v)
/* cmpxchg: stores `v` when the lvalue holds `cmp`; the value it held either way */
#define GEN_ATOMIC_CAS(lv, v, cmp) \
    (sizeof(lv) == 8 ? (long long)_InterlockedCompareExchange64((volatile long long *)&(lv), (long long)(v), (long long)(cmp)) \
     : sizeof(lv) == 4 ? (long long)_InterlockedCompareExchange((volatile long *)&(lv), (long)(v), (long)(cmp)) \
     : sizeof(lv) == 2 ? (long long)_InterlockedCompareExchange16((volatile short *)&(lv), (short)(v), (short)(cmp)) \
     : (long long)_InterlockedCompareExchange8((volatile char *)&(lv), (char)(v), (char)(cmp)))

/* Ghidra's `cpuid` pseudo-functions: the leaf's registers as Ghidra's x86 spec reads them back,
   EAX, EBX, EDX, ECX (per thread, as the instruction's registers are) */
static __inline unsigned int *gen_cpuid(int leaf) {
    static __declspec(thread) int regs[4];
    int r[4];
    __cpuidex(r, leaf, 0);
    regs[0] = r[0];
    regs[1] = r[1];
    regs[2] = r[3];
    regs[3] = r[2];
    return (unsigned int *)regs;
}
#define cpuid_basic_info(x) gen_cpuid(0)
#define cpuid_Version_info(x) gen_cpuid(1)
#define cpuid_cache_tlb_info(x) gen_cpuid(2)
#define cpuid_serial_info(x) gen_cpuid(3)
#define cpuid_Deterministic_Cache_Parameters_info(x) gen_cpuid(4)
#define cpuid_MONITOR_MWAIT_Features_info(x) gen_cpuid(5)
#define cpuid_Thermal_Power_Management_info(x) gen_cpuid(6)
#define cpuid_Extended_Feature_Enumeration_info(x) gen_cpuid(7)
#define cpuid_Direct_Cache_Access_info(x) gen_cpuid(9)
#define cpuid_Architectural_Performance_Monitoring_info(x) gen_cpuid(10)
#define cpuid_Extended_Topology_info(x) gen_cpuid(11)
#define cpuid_Processor_Extended_States_info(x) gen_cpuid(13)
#define cpuid_Quality_of_Service_info(x) gen_cpuid(15)
#define cpuid_brand_part1_info(x) gen_cpuid((int)0x80000002)
#define cpuid_brand_part2_info(x) gen_cpuid((int)0x80000003)
#define cpuid_brand_part3_info(x) gen_cpuid((int)0x80000004)

/* what a translated function reaches that is not translated: logged once by its address */
void GenUnimplemented(uint64_t address);

#endif
