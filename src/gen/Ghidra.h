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
#define SUB161(x, n) ((uint8_t)0)
#define SUB168(x, n) ((uint64_t)0)
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

/* what a translated function reaches that is not translated: logged once by its address */
void GenUnimplemented(uint64_t address);

#endif
