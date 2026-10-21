/*
** The types and helpers Ghidra's decompiler output is written in, so tools/translate.py can
** compile it as C. The generated files (src/gen/translated_*.c) are the reference's functions
** translated from build/corpus.jsonl; they run against the reference's own data sections,
** mapped at their original addresses (src/gen/RefImage.cpp), so a global is its address.
*/
#ifndef GEN_GHIDRA_H
#define GEN_GHIDRA_H

#include <float.h>
#include <malloc.h>
#include <process.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* curl's defaults name the C library's stream routines as values: Curl_open (FUN_14217a1f0)
   stores fwrite and fread as the write and read callbacks */
#include <stdio.h>
#include <string.h>
#include <time.h>
/* localeconv, the port's in place of the reference's FID-unnamed one (tools/translate.py
   CRT_BY_ADDR): undeclared, its pointer result would be cut to an int */
#include <locale.h>

/* Ghidra writes the C library's structs by their bare tag (`tm local_38;`) */
typedef struct tm tm;

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
/* Windows names the import prototypes bring in (tools/ghidra/ApplyFacts.java types each import
   slot from Ghidra's windows_vs12_64 archive), as tools/translate.py's WINDOWS_TYPES lists them.
   Scalars are the SDK's own; handles and pointers to structs the translation never looks inside
   are void *. Not under windows.h, which has its own. */
#ifndef _WINDEF_
typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned long long DWORD64;
typedef unsigned int UINT;
typedef int INT;
typedef long LONG;
typedef unsigned long ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef long HRESULT;
typedef long LSTATUS;
typedef unsigned long LCID;
typedef unsigned short ATOM;
typedef unsigned long long SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef unsigned long long DWORD_PTR;
typedef unsigned long long UINT_PTR;
typedef long long LONG_PTR;
typedef long long INT_PTR;
typedef long long LPARAM;
typedef unsigned long long WPARAM;
typedef long long LRESULT;
typedef unsigned long long SOCKET;
typedef unsigned short u_short;
typedef unsigned long u_long;
typedef unsigned int u_int;
typedef unsigned char u_char;
typedef int errno_t;
typedef size_t rsize_t;
typedef unsigned short wint_t;
typedef unsigned long long __uint64;
typedef void *HMODULE, *HINSTANCE, *HWND, *HDC, *HKEY, *HLOCAL, *HGLOBAL, *HCURSOR, *HICON, *HMENU,
    *HDROP, *HIMC, *HBRUSH;
typedef long long (*FARPROC)();
typedef void *LPVOID, *PVOID;
/* the LPC* names without their const: Ghidra types a local after the API parameter it ends up in,
   and the code may well have written through it first. SChannel's loader (FUN_141f74e30) builds
   the DLL path in a blz::wstring's buffer, terminates it (`mov word ptr [rdi + rax], r14w`,
   0x141f7525f) and hands it to LoadLibraryW: as const that store was C2166 and the loader was
   stubbed (2026-10-16). Nothing translated is checked against the SDK's declarations. */
typedef void *LPCVOID;
typedef char *LPSTR;
typedef char *LPCSTR;
typedef wchar_t *LPWSTR, *BSTR;
typedef wchar_t *LPCWSTR;
typedef DWORD *LPDWORD;
typedef BOOL *LPBOOL;
typedef BYTE *LPBYTE;
typedef long *LPLONG;
typedef void *LPCRITICAL_SECTION, *PSRWLOCK, *PCONDITION_VARIABLE, *LPSECURITY_ATTRIBUTES, *LPOVERLAPPED,
    *PCCRL_CONTEXT, *PCCERT_CONTEXT, *LPRECT, *LPPOINT, *LPUNKNOWN, *LPCGUID, *PRAWINPUTDEVICELIST,
    *PUNWIND_HISTORY_TABLE, *PFILETIME, *LPFILETIME, *PHKEY, *LPHANDLE, *PHANDLE, *HCERTSTORE,
    *PCCERT_CHAIN_CONTEXT, *LPSYSTEMTIME, *PSYSTEMTIME;
/* crypto handles (ULONG_PTR in the SDK) */
typedef unsigned long long HCRYPTHASH, HCRYPTPROV, HCRYPTKEY;
/* winsock's fd_set, as the code reaches it through pointers only */
typedef struct fd_set fd_set;
/* and the rest the import thunks and runtime helpers pass on (sockets, IME, DPAPI, BCrypt, the
   PE-section walk): handles and pointers, by their SDK width; the structs through pointers only
   (incomplete types, as fd_set). 26 functions were stubbed as "a type C does not know" (2026-10-18) */
typedef void *HKL, *HGDIOBJ, *LPWSADATA, *LPCANDIDATELIST, *LPCOMPOSITIONFORM, *LPCANDIDATEFORM,
    *PEXCEPTION_RECORD, *BCRYPT_ALG_HANDLE, *PSLIST_HEADER;
typedef unsigned char *PBYTE, *PUCHAR;
typedef unsigned short *LPWORD;
typedef unsigned int *PUINT;
typedef wchar_t *PCWSTR;
typedef long NTSTATUS;
/* winnt.h's DWORD of power-request flags, SetThreadExecutionState's (FUN_14205dfb0, 2026-10-19) */
typedef DWORD EXECUTION_STATE;
typedef unsigned short wctype_t;
typedef struct sockaddr sockaddr;
typedef struct timeval timeval;
typedef struct _DATA_BLOB DATA_BLOB;
typedef struct _CRYPTPROTECT_PROMPTSTRUCT CRYPTPROTECT_PROMPTSTRUCT;
/* the structs translated code fills and reads by field, by their SDK layout. SChannel's SSPI
   loader (FUN_141f74e30) asks GetVersionExA which DLL to load (dwOSVersionInfoSize = 0x94); it was
   stubbed for the type, so the SChannel factory never initialized and the Login click found no
   SSL factory (2026-10-15) */
typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
} _OSVERSIONINFOA;
/* the rest of the SDK structs Ghidra names in the corpus (each a stub until 2026-10-16: "a type C
   does not know"), by their SDK layout */
typedef unsigned long long DWORDLONG;
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} _FILETIME, FILETIME;
typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        long HighPart;
    };
    struct {
        DWORD LowPart;
        long HighPart;
    } u;
    long long QuadPart;
} _LARGE_INTEGER, LARGE_INTEGER;
typedef struct tagRECT {
    long left;
    long top;
    long right;
    long bottom;
} tagRECT;
typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            unsigned short wProcessorArchitecture;
            unsigned short wReserved;
        };
    };
    DWORD dwPageSize;
    void *lpMinimumApplicationAddress;
    void *lpMaximumApplicationAddress;
    unsigned long long dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    unsigned short wProcessorLevel;
    unsigned short wProcessorRevision;
} _SYSTEM_INFO, SYSTEM_INFO;
/* the image's own DOS header, which code reads to find its sections (e_lfanew at +0x3c) */
/* (as Ghidra lays it out: e_magic two chars, and the DOS stub after the header as e_program,
   which code reading a buffer Ghidra typed with it indexes, `param_3->e_magic[0]`) */
typedef struct _IMAGE_DOS_HEADER {
    char e_magic[2];
    unsigned short e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip,
        e_cs, e_lfarlc, e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10];
    long e_lfanew;
    unsigned char e_program[64];
} _IMAGE_DOS_HEADER, IMAGE_DOS_HEADER;
#endif
/* the UCRT's tag for FILE, as Ghidra names it */
typedef struct _iobuf _iobuf;
/* _onexit_t */
#include <stdlib.h>
/* _alloca: a reference function's stack allocation (`sub rsp, rax`) in the translated frame */
#include <malloc.h>
/* the reference's _setjmp(buf, frame) (FUN_1425bf1f0): the port's, on the translated function's
   own frame; the reference's longjmp is the port's by name. Same runtime, same jmp_buf */
#include <setjmp.h>
#define GEN_SETJMP(buf, frame) setjmp(*(jmp_buf *)(buf))
/* a call through `code *`: any arguments, the result in rax */
typedef unsigned long long code();

/* a reference global: its address in the mapped reference image */
#define REFG(type, addr) (*(type *)(uintptr_t)(addr##ULL))
#define REFP(addr) ((void *)(uintptr_t)(addr##ULL))
/* a call to a translated function, unprototyped: Ghidra's argument counts disagree between a
   function and its callers, and a definition earlier in the same file would otherwise impose its
   own (floats are promoted to double; the definitions take them that way) */
#define CALLF(type, f) ((type (*)())(f))
/* andps / xorps with the float sign masks, as tools/translate.py's float_masks rewrites them.
   Ghidra spells both an operand that is a float and one that already holds a float's bits as the
   same text; _Generic sees which it is: a float's bits are taken, an integer's are the bits */
static __inline unsigned int gen_f2bits(float f) { unsigned int u; memcpy(&u, &f, 4); return u; }
static __inline float gen_bits2f(unsigned int u) { float f; memcpy(&f, &u, 4); return f; }
static __inline unsigned int gen_u2bits(unsigned long long v) { return (unsigned int)v; }
#define GEN_F32BITS(x) _Generic((x), float: gen_f2bits, double: gen_f2bits, default: gen_u2bits)(x)
/* a double's 64 bits as an integer (an integer as it is): what an 8-byte slot holds, whatever
   the decompiler typed it (tools/translate.py int_variants()) */
static __inline long long gen_d2q(double d) { long long q; memcpy(&q, &d, 8); return q; }
static __inline long long gen_q2q(long long q) { return q; }
static __inline double gen_q2d(long long q) { double d; memcpy(&d, &q, 8); return d; }
#define GEN_D2Q(x) _Generic((x), double: gen_d2q, float: gen_d2q, default: gen_q2q)(x)
#define GEN_FABSBITS(x) (GEN_F32BITS(x) & 0x7fffffffu)
#define GEN_FNEGBITS(x) (GEN_F32BITS(x) ^ 0x80000000u)
#define GEN_FABS(x) gen_bits2f(GEN_FABSBITS(x))
#define GEN_FNEG(x) gen_bits2f(GEN_FNEGBITS(x))
/* a translated function's call count, in its own file's table (GEN_HITS is that file's
   gen_hits_NNN, listed in hits.c; written out by RefImage.cpp under FROST_HITS). Per file, so a
   function moving in or out of one file renumbers no other */
#define GEN_HIT(i) (GEN_HITS[i]++)
/* a floating point argument Ghidra spelled through an integer (`*param_3` of an undefined8 *):
   its bits, not its value - an unprototyped call would pass the integer in a general register
   while the callee reads xmm */
static __inline double GEN_AS_DOUBLE(unsigned long long bits) {
    union { unsigned long long u; double d; } v;
    v.u = bits;
    return v.d;
}
static __inline double GEN_AS_FLOAT(unsigned long long bits) {
    union { unsigned int u; float f; } v;
    v.u = (unsigned int)bits;
    return (double)v.f;
}
/* an argument for a floating point parameter, by the argument's own C type: a float or double
   goes by value, anything else (an integer Ghidra spelled for a register that holds one) by its
   bits. The translator could not always see the type in the text (`((float *)(gen_frame +
   0x40))[0]` went through `(unsigned long long)` and arrived as 14 -> 2e-44: C_Console's font
   height was 0 and the console's EditBox refused it, 2026-10-21); C's _Generic can */
static __inline double gen_farg_f(float x) { return (double)x; }
static __inline double gen_farg_d(double x) { return (double)(float)x; }
static __inline double gen_farg_u(unsigned long long bits) { return GEN_AS_FLOAT(bits); }
static __inline double gen_darg_f(float x) { return (double)x; }
static __inline double gen_darg_d(double x) { return x; }
static __inline double gen_darg_u(unsigned long long bits) { return GEN_AS_DOUBLE(bits); }
#define GEN_ARG_FLOAT(x) _Generic((x), float: gen_farg_f, double: gen_farg_d, default: gen_farg_u)(x)
#define GEN_ARG_DOUBLE(x) _Generic((x), float: gen_darg_f, double: gen_darg_d, default: gen_darg_u)(x)

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
/* the rest of CONCATab with a + b <= 8: a bytes over b bytes */
#define GEN_CONCAT(a, na, b, nb) ((uint64_t)((((uint64_t)(a) & (~0ULL >> (64 - 8 * (na)))) << (8 * (nb))) | \
    ((uint64_t)(b) & (~0ULL >> (64 - 8 * (nb))))))
#define CONCAT23(a, b) GEN_CONCAT(a, 2, b, 3)
#define CONCAT25(a, b) GEN_CONCAT(a, 2, b, 5)
#define CONCAT33(a, b) GEN_CONCAT(a, 3, b, 3)
#define CONCAT34(a, b) GEN_CONCAT(a, 3, b, 4)
#define CONCAT43(a, b) GEN_CONCAT(a, 4, b, 3)
#define CONCAT51(a, b) GEN_CONCAT(a, 5, b, 1)
#define CONCAT52(a, b) GEN_CONCAT(a, 5, b, 2)
#define CONCAT61(a, b) GEN_CONCAT(a, 6, b, 1)
#define SUB41(x, n) ((uint8_t)((uint32_t)(x) >> ((n) * 8)))
#define SUB42(x, n) ((uint16_t)((uint32_t)(x) >> ((n) * 8)))
#define SUB81(x, n) ((uint8_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB82(x, n) ((uint16_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB84(x, n) ((uint32_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB21(x, n) ((uint8_t)((uint16_t)(x) >> ((n) * 8)))
/* the odd widths, held in the next C type up and masked to their bytes */
#define SUB43(x, n) ((uint32_t)(((uint32_t)(x) >> ((n) * 8)) & 0xffffffu))
#define SUB83(x, n) ((uint32_t)(((uint64_t)(x) >> ((n) * 8)) & 0xffffffu))
#define SUB85(x, n) ((uint64_t)(((uint64_t)(x) >> ((n) * 8)) & 0xffffffffffull))
#define SUB86(x, n) ((uint64_t)(((uint64_t)(x) >> ((n) * 8)) & 0xffffffffffffull))
/* (a 6-byte value's 2 bytes at n, FUN_1423f32a0; it did not link, 2026-10-18) */
#define SUB62(x, n) ((uint16_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB61(x, n) ((uint8_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB64(x, n) ((uint32_t)((uint64_t)(x) >> ((n) * 8)))
#define SUB87(x, n) ((uint64_t)(((uint64_t)(x) >> ((n) * 8)) & 0xffffffffffffffull))
/* SUBab with b wider than a: Ghidra's spelling of a narrow value passed in a wider register, its
   own bytes and zeros above (`SUB14(param_2 != 0,0)`, the bool DragAcceptFiles takes as a BOOL in
   FUN_142041da0; it did not link, 2026-10-19) */
#define SUB12(x, n) ((uint16_t)(uint8_t)((uint8_t)(x) >> ((n) * 8)))
#define SUB14(x, n) ((uint32_t)(uint8_t)((uint8_t)(x) >> ((n) * 8)))
#define SUB18(x, n) ((uint64_t)(uint8_t)((uint8_t)(x) >> ((n) * 8)))
#define SUB24(x, n) ((uint32_t)(uint16_t)((uint16_t)(x) >> ((n) * 8)))
#define SUB28(x, n) ((uint64_t)(uint16_t)((uint16_t)(x) >> ((n) * 8)))
#define SUB48(x, n) ((uint64_t)(uint32_t)((uint32_t)(x) >> ((n) * 8)))
#define ZEXT14(x) ((uint32_t)(uint8_t)(x))
#define ZEXT18(x) ((uint64_t)(uint8_t)(x))
#define ZEXT24(x) ((uint32_t)(uint16_t)(x))
#define ZEXT28(x) ((uint64_t)(uint16_t)(x))
#define ZEXT48(x) ((uint64_t)(uint32_t)(x))
#define ZEXT816(x) ((uint64_t)(x))
/* (to 12 bytes: a value no wider than 8 there, which a 12-byte store then takes whole) */
#define ZEXT412(x) ((uint64_t)(uint32_t)(x))
#define ZEXT812(x) ((uint64_t)(x))
/* (and the other widths up to 8, `auVar2._0_12_ = ZEXT712(0)`: 10 functions did not link,
   2026-10-18; and the odd widths to 8, masked to their bytes) */
#define ZEXT112(x) ((uint64_t)(uint8_t)(x))
#define ZEXT212(x) ((uint64_t)(uint16_t)(x))
#define ZEXT312(x) ((uint64_t)(x) & 0xffffffull)
#define ZEXT512(x) ((uint64_t)(x) & 0xffffffffffull)
#define ZEXT612(x) ((uint64_t)(x) & 0xffffffffffffull)
#define ZEXT712(x) ((uint64_t)(x) & 0xffffffffffffffull)
#define ZEXT38(x) ((uint64_t)(x) & 0xffffffull)
#define ZEXT58(x) ((uint64_t)(x) & 0xffffffffffull)
#define ZEXT68(x) ((uint64_t)(x) & 0xffffffffffffull)
#define ZEXT78(x) ((uint64_t)(x) & 0xffffffffffffffull)
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
#define SBORROW1(a, b) ((((int8_t)(a) ^ (int8_t)(b)) & ((int8_t)(a) ^ (int8_t)((uint8_t)(a) - (uint8_t)(b)))) < 0)
#define SBORROW2(a, b) ((((int16_t)(a) ^ (int16_t)(b)) & ((int16_t)(a) ^ (int16_t)((uint16_t)(a) - (uint16_t)(b)))) < 0)
#define SBORROW8(a, b) ((((int64_t)(a) ^ (int64_t)(b)) & ((int64_t)(a) ^ (int64_t)((uint64_t)(a) - (uint64_t)(b)))) < 0)
#define POPCOUNT(x) ((int)__popcnt64((uint64_t)(x)))
#define NAN_FLOAT(x) ((x) != (x))
/* Ghidra's `NAN(x)` is a test, the C library's `NAN` the constant (`local_48 = -NAN;`): the
   translator spells the test GEN_ISNAN, and math.h keeps NAN. Redefined here instead, math.h
   below took it back and every test failed to compile (C2064, 2026-10-16) */
#define GEN_ISNAN(x) ((x) != (x))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#include <math.h>
#define SQRT(x) sqrt(x)
/* Ghidra's rounding pseudo-ops: the instruction's own (round to nearest in the default mode) */
#define ROUND(x) nearbyint(x)
#define TRUNC(x) trunc(x)
#define FLOOR(x) floor(x)
#define CEIL(x) ceil(x)

/* a `lock`-prefixed instruction (Ghidra's LOCK(); ... UNLOCK();), by the size of the lvalue it
   works on; each yields the value before the operation, as the instruction leaves it */
#include <intrin.h>
#define GEN_ATOMIC_(op, lv, v) \
    (sizeof(lv) == 8 ? (long long)_Interlocked##op##64((volatile long long *)&(lv), (long long)(v)) \
     : sizeof(lv) == 4 ? (long long)_Interlocked##op((volatile long *)&(lv), (long)(long long)(v)) \
     : sizeof(lv) == 2 ? (long long)_Interlocked##op##16((volatile short *)&(lv), (short)(long long)(v)) \
     : (long long)_Interlocked##op##8((volatile char *)&(lv), (char)(long long)(v)))
/* a piece of a variable 3, 5, 6 or 7 bytes wide (`local_1e4._0_3_`), read and written in place */
static __inline unsigned long long GEN_PIECE(const void *p, int off, int n) {
    unsigned long long x = 0;
    memcpy(&x, (const char *)p + off, n);
    return x;
}
#define GEN_PIECE_SET(p, off, n, v) do { unsigned long long gen_piece_ = (unsigned long long)(v); \
    memcpy((char *)(p) + (off), &gen_piece_, (n)); } while (0)
/* the same as an expression (a store of a value's low n bytes inside a comma expression,
   `(GEN_PIECE(&local_8, 0, 7) = SUB87(dVar1,0), ...)`, C2106, 2026-10-18), and a value's bits:
   a double's or a float's as they are in memory, an integer's as it is */
static __inline unsigned long long gen_piece_set(void *p, int off, int n, unsigned long long v) {
    memcpy((char *)p + off, &v, n);
    return v;
}
static __inline unsigned long long gen_d2bits(double x) { unsigned long long r; memcpy(&r, &x, 8); return r; }
/* the other way: an integer's bits as a double (a register Ghidra typed double that holds an
   integer, tools/translate.py reinterpreted_float_locals) */
static __inline double gen_bits2d(unsigned long long u) { double d; memcpy(&d, &u, 8); return d; }
static __inline unsigned long long gen_fl2bits(float x) { unsigned int r; memcpy(&r, &x, 4); return r; }
static __inline unsigned long long gen_u2bits64(unsigned long long x) { return x; }
#define GEN_BITS(x) _Generic((x), double: gen_d2bits, float: gen_fl2bits, default: gen_u2bits64)(x)
/* a switch on a double (`case 0.0:`, which C refuses, C2052): its bits, -0.0 as 0.0, so a case
   matches exactly the values == would */
static __inline unsigned long long gen_dkey(double x) { return x == 0.0 ? 0ull : gen_d2bits(x); }
/* ---- 16-byte values (tools/v16.py) ----------------------------------------------------------
   Ghidra's `undefined1 auVar [16]`, an SSE register, as a union C can assign, pass and return.
   Integer operators Ghidra writes on one are 128-bit arithmetic (gen_v16_add ...); its SSE
   pseudo-ops are the intrinsics (v16.PSEUDO). A scalar moves in and out by its bits: Ghidra
   moves a float into a register with movd, not a conversion */
typedef union gen_v16 {
    __m128i m;
    __m128 f;
    __m128d d;
    unsigned char b[16];
    unsigned short w[8];
    unsigned int u[4];
    unsigned long long q[2];
} gen_v16;
/* 32- and 64-byte values (an AVX2 / AVX-512 register): storage only, tools/translate.py's
   wide_vectors(). Their pieces are read and written in place, and any operation on one is a wide
   path the translation does not take (the CPU-feature bits in the reference's data stay clear):
   GenWidePath, logged as an unimplemented address should it ever run */
typedef struct gen_v32 { __declspec(align(16)) unsigned char b[32]; } gen_v32;
typedef struct gen_v64 { __declspec(align(16)) unsigned char b[64]; } gen_v64;
#define GenWidePath(a) GenUnimplemented(a)
static __inline gen_v16 gen_v16_qq(unsigned long long lo, unsigned long long hi) {
    gen_v16 r; r.q[0] = lo; r.q[1] = hi; return r;
}
static __inline gen_v16 gen_v16_m(__m128i m) { gen_v16 r; r.m = m; return r; }
static __inline gen_v16 gen_v16_f(__m128 f) { gen_v16 r; r.f = f; return r; }
static __inline gen_v16 gen_v16_d(__m128d d) { gen_v16 r; r.d = d; return r; }
static __inline gen_v16 gen_v16_fromq(unsigned long long x) { return gen_v16_qq(x, 0); }
static __inline gen_v16 gen_v16_fromf(float x) { gen_v16 r = gen_v16_qq(0, 0); memcpy(&r, &x, 4); return r; }
static __inline gen_v16 gen_v16_fromd(double x) { gen_v16 r = gen_v16_qq(0, 0); memcpy(&r, &x, 8); return r; }
/* (a 16-byte value is itself: `gen_v16_u(CALLF(gen_v16, FUN_1412c9f10)(...))`, a call already
   returning one, C2440 in 2 functions, 2026-10-18) */
static __inline gen_v16 gen_v16_same(gen_v16 v) { return v; }
#define gen_v16_u(x) _Generic((x), float: gen_v16_fromf, double: gen_v16_fromd, \
    gen_v16: gen_v16_same, default: gen_v16_fromq)(x)
static __inline unsigned long long gen_v16_q0(gen_v16 v) { return v.q[0]; }
static __inline float gen_v16_f0(gen_v16 v) { float f; memcpy(&f, &v, 4); return f; }
static __inline double gen_v16_d0(gen_v16 v) { double d; memcpy(&d, &v, 8); return d; }
/* a scalar taken from one, as the variable it is stored in reads it */
#define GEN_V16_AS(like, v) _Generic((like), float: gen_v16_f0(v), double: gen_v16_d0(v), \
    default: gen_v16_q0(v))
static __inline gen_v16 gen_v16_and(gen_v16 a, gen_v16 b) { return gen_v16_m(_mm_and_si128(a.m, b.m)); }
static __inline gen_v16 gen_v16_or(gen_v16 a, gen_v16 b) { return gen_v16_m(_mm_or_si128(a.m, b.m)); }
static __inline gen_v16 gen_v16_xor(gen_v16 a, gen_v16 b) { return gen_v16_m(_mm_xor_si128(a.m, b.m)); }
static __inline gen_v16 gen_v16_not(gen_v16 a) { return gen_v16_qq(~a.q[0], ~a.q[1]); }
static __inline gen_v16 gen_v16_add(gen_v16 a, gen_v16 b) {
    unsigned long long lo = a.q[0] + b.q[0];
    return gen_v16_qq(lo, a.q[1] + b.q[1] + (lo < a.q[0]));
}
static __inline gen_v16 gen_v16_sub(gen_v16 a, gen_v16 b) {
    return gen_v16_qq(a.q[0] - b.q[0], a.q[1] - b.q[1] - (a.q[0] < b.q[0]));
}
static __inline gen_v16 gen_v16_neg(gen_v16 a) { return gen_v16_sub(gen_v16_qq(0, 0), a); }
/* the low 128 bits of the product */
static __inline gen_v16 gen_v16_mul(gen_v16 a, gen_v16 b) {
    unsigned long long hi;
    unsigned long long lo = _umul128(a.q[0], b.q[0], &hi);
    return gen_v16_qq(lo, hi + a.q[0] * b.q[1] + a.q[1] * b.q[0]);
}
static __inline gen_v16 gen_v16_shl(gen_v16 a, int n) {
    if (n <= 0) return a;
    if (n >= 128) return gen_v16_qq(0, 0);
    if (n >= 64) return gen_v16_qq(0, a.q[0] << (n - 64));
    return gen_v16_qq(a.q[0] << n, (a.q[1] << n) | (a.q[0] >> (64 - n)));
}
static __inline gen_v16 gen_v16_shr(gen_v16 a, int n) {
    if (n <= 0) return a;
    if (n >= 128) return gen_v16_qq(0, 0);
    if (n >= 64) return gen_v16_qq(a.q[1] >> (n - 64), 0);
    return gen_v16_qq((a.q[0] >> n) | (a.q[1] << (64 - n)), a.q[1] >> n);
}
static __inline int gen_v16_eq(gen_v16 a, gen_v16 b) { return a.q[0] == b.q[0] && a.q[1] == b.q[1]; }
static __inline int gen_v16_lt(gen_v16 a, gen_v16 b) {
    return a.q[1] < b.q[1] || (a.q[1] == b.q[1] && a.q[0] < b.q[0]);
}
static __inline int gen_v16_gt(gen_v16 a, gen_v16 b) { return gen_v16_lt(b, a); }
static __inline int gen_v16_zero(gen_v16 a) { return (a.q[0] | a.q[1]) == 0; }
/* unsigned 128-bit division: by shift and subtract (Ghidra writes it for a 64-bit divide by a
   constant done through a 128-bit product, which wide_multiply takes before it gets here) */
static __inline void gen_v16_divmod(gen_v16 a, gen_v16 b, gen_v16 *q, gen_v16 *r) {
    gen_v16 quo = gen_v16_qq(0, 0), rem = gen_v16_qq(0, 0);
    int i;
    if (b.q[1] == 0 && a.q[1] == 0) {
        *q = gen_v16_qq(b.q[0] ? a.q[0] / b.q[0] : 0, 0);
        *r = gen_v16_qq(b.q[0] ? a.q[0] % b.q[0] : 0, 0);
        return;
    }
    for (i = 127; i >= 0; i--) {
        rem = gen_v16_shl(rem, 1);
        rem.q[0] |= (i >= 64 ? a.q[1] >> (i - 64) : a.q[0] >> i) & 1;
        if (!gen_v16_lt(rem, b)) {
            rem = gen_v16_sub(rem, b);
            if (i >= 64) quo.q[1] |= 1ull << (i - 64); else quo.q[0] |= 1ull << i;
        }
    }
    *q = quo;
    *r = rem;
}
static __inline gen_v16 gen_v16_div(gen_v16 a, gen_v16 b) { gen_v16 q, r; gen_v16_divmod(a, b, &q, &r); return q; }
static __inline gen_v16 gen_v16_mod(gen_v16 a, gen_v16 b) { gen_v16 q, r; gen_v16_divmod(a, b, &q, &r); return r; }
/* ZEXTn16 / SEXTn16 of a value already held in one: its low n bytes, zero or sign extended */
static __inline gen_v16 gen_v16_zext(gen_v16 a, int n) {
    gen_v16 r = gen_v16_qq(0, 0);
    memcpy(&r, &a, n < 16 ? n : 16);
    return r;
}
static __inline gen_v16 gen_v16_sext(gen_v16 a, int n) {
    gen_v16 r = gen_v16_zext(a, n);
    if (n < 16 && (a.b[n - 1] & 0x80)) memset(r.b + n, 0xff, 16 - n);
    return r;
}
/* CONCATab with a + b == 16: hi above the low `lo_bytes` of lo */
static __inline gen_v16 gen_v16_concat(gen_v16 hi, gen_v16 lo, int lo_bytes) {
    gen_v16 r = hi;
    gen_v16 h = gen_v16_shl(hi, lo_bytes * 8);
    r = gen_v16_or(h, gen_v16_zext(lo, lo_bytes));
    return r;
}
/* SUB16n(v, k): n bytes at offset k */
static __inline unsigned long long gen_v16_bytes(gen_v16 v, int k, int n) {
    unsigned long long x = 0;
    if (k >= 0 && k < 16) memcpy(&x, v.b + k, (n < 16 - k ? n : 16 - k) < 8 ? (n < 16 - k ? n : 16 - k) : 8);
    return x;
}
/* a piece wider than 8 bytes (`auVar1._4_12_`), read or written in place */
static __inline gen_v16 gen_v16_load(const void *p, int n) {
    gen_v16 r = gen_v16_qq(0, 0);
    memcpy(&r, p, n < 16 ? n : 16);
    return r;
}
static __inline void gen_v16_store(void *p, int n, gen_v16 v) { memcpy(p, &v, n < 16 ? n : 16); }
/* vpmullq on 128 bits (AVX-512VL): each 64-bit lane's product, low 64 bits; in plain C, so it
   runs where AVX-512 does not */
static __inline __m128i gen_mullo_epi64(__m128i a, __m128i b) {
    gen_v16 x, y, r;
    x.m = a;
    y.m = b;
    r.q[0] = x.q[0] * y.q[0];
    r.q[1] = x.q[1] * y.q[1];
    return r.m;
}
/* movmskps / movmskpd as Ghidra spells them, `movmskpd(dest, vector)`: the lanes' sign bits */
static __inline unsigned int movmskps(unsigned long long dest, gen_v16 v) {
    (void)dest;
    return (unsigned int)_mm_movemask_ps(v.f);
}
static __inline unsigned int movmskpd(unsigned long long dest, gen_v16 v) {
    (void)dest;
    return (unsigned int)_mm_movemask_pd(v.d);
}
/* Ghidra's rdrand pseudo-op and its carry flag (the CRT's random seeding, FUN_14026b280): the
   intrinsic, whose success is the flag. Did not link (3 functions, 2026-10-18) */
static __declspec(thread) int gen_rdrand_ok;
static __inline unsigned long long rdrand(void) {
    unsigned long long v = 0;
    gen_rdrand_ok = _rdrand64_step(&v);
    return v;
}
static __inline int rdrandIsValid(void) { return gen_rdrand_ok; }
/* pavgb / pavgw as Ghidra writes one lane of them, on scalars (`pavgb((char)uVar1, *p)`): the
   rounded average of the two, unsigned (did not link, 2026-10-18) */
static __inline unsigned char pavgb(unsigned int a, unsigned int b) {
    return (unsigned char)(((a & 0xffu) + (b & 0xffu) + 1u) >> 1);
}
static __inline unsigned short pavgw(unsigned int a, unsigned int b) {
    return (unsigned short)(((a & 0xffffu) + (b & 0xffffu) + 1u) >> 1);
}

/* Ghidra's `(code *)swi(n)`, called: the trap the instruction raises. `int3` (swi(3)) breaks
   into a debugger; `int 0x29` (swi(0x29)) is __fastfail, which ends the process. Ghidra drops
   the code a __fastfail passes in ecx, so it is reported as a generic one (7,
   FAST_FAIL_FATAL_APP_EXIT). 8 functions were stubbed as "bad data or a trap" for these */
static unsigned long long gen_swi_break(void) { __debugbreak(); return 0; }
static unsigned long long gen_swi_fastfail(void) { __fastfail(7); return 0; }
#define swi(n) ((n) == 0x29 ? gen_swi_fastfail : gen_swi_break)
/* `ud2`, which Ghidra spells `(code *)invalidInstructionException()` and calls: the same fault */
static unsigned long long gen_ud2(void) { __ud2(); }
#define invalidInstructionException() gen_ud2
#define GEN_ATOMIC_ADD(lv, v) GEN_ATOMIC_(ExchangeAdd, lv, v)
#define GEN_ATOMIC_AND(lv, v) GEN_ATOMIC_(And, lv, v)
#define GEN_ATOMIC_OR(lv, v) GEN_ATOMIC_(Or, lv, v)
#define GEN_ATOMIC_XOR(lv, v) GEN_ATOMIC_(Xor, lv, v)
#define GEN_ATOMIC_XCHG(lv, v) GEN_ATOMIC_(Exchange, lv, v)
/* cmpxchg: stores `v` when the lvalue holds `cmp`; the value it held either way */
/* a locked instruction Ghidra kept nothing of (`lock or [rsp], 0`): a full memory fence */
#define GEN_FENCE() __faststorefence()
/* (every width's branch is compiled and only lv's runs: the narrow ones convert through long long,
   so a pointer lvalue's 8-byte case does not warn of truncation in branches it never takes) */
#define GEN_ATOMIC_CAS(lv, v, cmp) \
    (sizeof(lv) == 8 ? (long long)_InterlockedCompareExchange64((volatile long long *)&(lv), (long long)(v), (long long)(cmp)) \
     : sizeof(lv) == 4 ? (long long)_InterlockedCompareExchange((volatile long *)&(lv), (long)(long long)(v), (long)(long long)(cmp)) \
     : sizeof(lv) == 2 ? (long long)_InterlockedCompareExchange16((volatile short *)&(lv), (short)(long long)(v), (short)(long long)(cmp)) \
     : (long long)_InterlockedCompareExchange8((volatile char *)&(lv), (char)(long long)(v), (char)(long long)(cmp)))

/* Ghidra's `cpuid` pseudo-functions: the leaf's registers as Ghidra's x86 spec reads them back,
   EAX, EBX, EDX, ECX (per thread, as the instruction's registers are). Each call gets its own
   slot of a small ring: code holds two leaves' results at once (the brand string reads leaf
   0x80000003's after asking for 0x80000004, FUN_141dfa970), which one shared buffer garbled */
static __inline unsigned int *gen_cpuid(int leaf) {
    static __declspec(thread) int regs[8][4];
    static __declspec(thread) unsigned int next;
    int *slot = regs[next++ & 7];
    int r[4];
    __cpuidex(r, leaf, 0);
    slot[0] = r[0];
    slot[1] = r[1];
    slot[2] = r[3];
    slot[3] = r[2];
    return (unsigned int *)slot;
}
/* Ghidra's `xinuse(0)`: xgetbv with ecx = 1, the state components in use */
#define xinuse(x) _xgetbv(1)
/* (any other leaf, `cpuid(0x80000000)`, the same) */
#define cpuid(x) gen_cpuid((int)(x))
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

/* MSVC's array construction helpers, as Ghidra names them: each element in turn (destruction in
   reverse), the element function called with its address */
static __inline unsigned long long gen_vector_iterate(void *p, unsigned long long size, unsigned long long count,
                                                      code *each, int reverse) {
    unsigned long long i;
    for (i = 0; i < count; i++) {
        unsigned long long k = reverse ? count - 1 - i : i;
        ((unsigned long long (*)(void *))each)((char *)p + k * size);
    }
    return 0;
}
#define _vector_constructor_iterator_(p, size, count, each, ...) gen_vector_iterate((void *)(p), (size), (count), (code *)(each), 0)
#define _eh_vector_constructor_iterator_(p, size, count, each, ...) gen_vector_iterate((void *)(p), (size), (count), (code *)(each), 0)
#define _vector_destructor_iterator_(p, size, count, each, ...) gen_vector_iterate((void *)(p), (size), (count), (code *)(each), 1)
#define _eh_vector_destructor_iterator_(p, size, count, each, ...) gen_vector_iterate((void *)(p), (size), (count), (code *)(each), 1)

/* function-local statics: the port runtime's guard protocol (vcruntime's thread_safe_statics) */
/* (read through RefImage.cpp: the C compiler crashes on the thread-local itself) */
int GenInitThreadEpoch(void);
#define _Init_thread_epoch GenInitThreadEpoch()
int *GenInitThreadEpochPtr(void);
/* the reference's thread-local block for this thread (RefImage.cpp): `ThreadLocalStoragePointer
   [_tls_index]` as the loader would have built it from the image's TLS template */
void *GenRefTlsBlock(void);
/* and the names Ghidra reaches it by, whatever shape the access takes (`pvVar = ThreadLocal
   StoragePointer; uVar = _tls_index; ... *(longlong *)((longlong)pvVar + uVar * 8)`): a
   one-slot array holding that block, at index 0 */
void **GenRefTlsSlots(void);
#define ThreadLocalStoragePointer ((void *)GenRefTlsSlots())
#define _tls_index 0u
void __cdecl _Init_thread_header(int *guard);
void __cdecl _Init_thread_footer(int *guard);

/* the image's destructors are never registered: the quit path leaves without running static
   destructors, the port's as the reference's */
#define atexit(f) ((void)(f), 0)

/* what a translated function reaches that is not translated: logged once by its address */
void GenUnimplemented(uint64_t address);

#endif
