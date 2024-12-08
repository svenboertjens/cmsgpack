#ifndef MASKS_H
#define MASKS_H

///////////////////
//     MASKS     //
///////////////////

// The smallest non-fixlen masks are used for identifying types during decoding (such as DT_STR_SHORT for strings)

// Integers
#define DT_UINT_FIXED 0x00ULL
#define DT_UINT_BIT08 0xCCULL
#define DT_UINT_BIT16 0xCDULL
#define DT_UINT_BIT32 0xCEULL
#define DT_UINT_BIT64 0xCFULL

#define UINT_FIXED_MAXVAL 0x7FLL
#define UINT_BIT08_MAXVAL 0xFFULL
#define UINT_BIT16_MAXVAL 0xFFFFULL
#define UINT_BIT32_MAXVAL 0xFFFFFFFFULL
#define UINT_BIT64_MAXVAL 0xFFFFFFFFFFFFFFFFULL

#define DT_INT_FIXED 0xE0ULL
#define DT_INT_BIT08 0xD0ULL
#define DT_INT_BIT16 0xD1ULL
#define DT_INT_BIT32 0xD2ULL
#define DT_INT_BIT64 0xD3ULL

#define INT_FIXED_MAXVAL -32LL
#define INT_BIT08_MAXVAL -128LL
#define INT_BIT16_MAXVAL -32768LL
#define INT_BIT32_MAXVAL -2147483648LL
#define INT_BIT64_MAXVAL -9223372036854775808LL

// Floats
#define DT_FLOAT_BIT32 0xCAULL
#define DT_FLOAT_BIT64 0xCBULL

// Strings
#define DT_STR_FIXED 0xA0ULL
#define DT_STR_SHORT 0xD9ULL
#define DT_STR_MEDIUM 0xDAULL
#define DT_STR_LARGE 0xDBULL

#define STR_FIXED_MAXSIZE 0x1FULL
#define STR_SHORT_MAXSIZE 0xFFULL
#define STR_MEDIUM_MAXSIZE 0xFFFFULL
#define STR_LARGE_MAXSIZE 0xFFFFFFFFULL

// Arrays
#define DT_ARR_FIXED 0x90ULL
#define DT_ARR_MEDIUM 0xDCULL
#define DT_ARR_LARGE 0xDDULL

#define ARR_FIXED_MAXITEMS 0x0FULL
#define ARR_MEDIUM_MAXITEMS 0xFFFFULL
#define ARR_LARGE_MAXITEMS 0xFFFFFFFFULL

// Maps
#define DT_MAP_FIXED 0x80ULL
#define DT_MAP_MEDIUM 0xDEULL
#define DT_MAP_LARGE 0xDFULL

#define MAP_FIXED_MAXPAIRS 0x0FULL
#define MAP_MEDIUM_MAXPAIRS 0xFFFFULL
#define MAP_LARGE_MAXPAIRS 0xFFFFFFFFULL

// States
#define DT_NIL   0xC0ULL
#define DT_TRUE  0xC3ULL
#define DT_FALSE 0xC2ULL

// Binary
#define DT_BIN_SHORT 0xC4ULL
#define DT_BIN_MEDIUM 0xC5ULL
#define DT_BIN_LARGE 0xC6ULL

#define BIN_SHORT_MAXSIZE 0x0FULL
#define BIN_MEDIUM_MAXSIZE 0xFFFFULL
#define BIN_LARGE_MAXSIZE 0xFFFFFFFFULL

// Extension Types
#define DT_EXT_FIX1 0xD4ULL
#define DT_EXT_FIX2 0xD5ULL
#define DT_EXT_FIX4 0xD6ULL
#define DT_EXT_FIX8 0xD7ULL
#define DT_EXT_FIX16 0xD8ULL

#define DT_EXT_SHORT 0xC7ULL
#define DT_EXT_MEDIUM 0xC8ULL
#define DT_EXT_LARGE 0xC9ULL

#define EXT_SHORT_MAXSIZE 0xFFULL
#define EXT_MEDIUM_MAXSIZE 0xFFFFULL
#define EXT_LARGE_MAXSIZE 0xFFFFFFFFULL

#endif // MASKS_H