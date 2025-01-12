#ifndef MASKS_H
#define MASKS_H

///////////////////
//     MASKS     //
///////////////////

// Small is 1 byte
#define LIMIT_SMALL 0xFF

// Medium is 2 bytes
#define LIMIT_MEDIUM 0xFFFF

// Large is 4 bytes
#define LIMIT_LARGE 0xFFFFFFFF

// Limits of ints and uints as strings
#define LIMIT_UINT_STR "18446744073709551615" // Max positive integer (unsigned)
#define LIMIT_INT_STR  "-9223372036854775808" // Min negative integer (signed)

// Integers
#define DT_UINT_FIXED 0x00ULL
#define DT_UINT_BIT8 0xCCULL
#define DT_UINT_BIT16 0xCDULL
#define DT_UINT_BIT32 0xCEULL
#define DT_UINT_BIT64 0xCFULL

#define LIMIT_UINT_FIXED 0x7FLL
#define LIMIT_UINT_BIT8  0xFFULL
#define LIMIT_UINT_BIT16 0xFFFFULL
#define LIMIT_UINT_BIT32 0xFFFFFFFFULL
#define LIMIT_UINT_BIT64 0xFFFFFFFFFFFFFFFFULL

#define DT_INT_FIXED 0xE0ULL
#define DT_INT_BIT8 0xD0ULL
#define DT_INT_BIT16 0xD1ULL
#define DT_INT_BIT32 0xD2ULL
#define DT_INT_BIT64 0xD3ULL

#define LIMIT_INT_FIXED -32LL
#define LIMIT_INT_BIT8  -128LL
#define LIMIT_INT_BIT16 -32768LL
#define LIMIT_INT_BIT32 -2147483648LL
#define LIMIT_INT_BIT64 -9223372036854775808LL

// Floats
#define DT_FLOAT_BIT32 0xCAULL
#define DT_FLOAT_BIT64 0xCBULL

// Strings
#define DT_STR_FIXED 0xA0ULL
#define DT_STR_SMALL 0xD9ULL
#define DT_STR_MEDIUM 0xDAULL
#define DT_STR_LARGE 0xDBULL

#define LIMIT_STR_FIXED 0x1FULL

// Arrays
#define DT_ARR_FIXED 0x90ULL
#define DT_ARR_MEDIUM 0xDCULL
#define DT_ARR_LARGE 0xDDULL

#define LIMIT_ARR_FIXED 0x0FULL

// Maps
#define DT_MAP_FIXED 0x80ULL
#define DT_MAP_MEDIUM 0xDEULL
#define DT_MAP_LARGE 0xDFULL

#define LIMIT_MAP_FIXED 0x0FULL

// States
#define DT_NIL   0xC0ULL
#define DT_TRUE  0xC3ULL
#define DT_FALSE 0xC2ULL

// Binary
#define DT_BIN_SMALL 0xC4ULL
#define DT_BIN_MEDIUM 0xC5ULL
#define DT_BIN_LARGE 0xC6ULL

// Extension Types
#define DT_EXT_FIX1 0xD4ULL
#define DT_EXT_FIX2 0xD5ULL
#define DT_EXT_FIX4 0xD6ULL
#define DT_EXT_FIX8 0xD7ULL
#define DT_EXT_FIX16 0xD8ULL

#define DT_EXT_SMALL 0xC7ULL
#define DT_EXT_MEDIUM 0xC8ULL
#define DT_EXT_LARGE 0xC9ULL

#endif // MASKS_H