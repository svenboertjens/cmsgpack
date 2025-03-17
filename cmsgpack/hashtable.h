#ifndef CMSGPACK_HASHTABLE_H
#define CMSGPACK_HASHTABLE_H


#include <stdint.h>
#include <stdbool.h>
#include "internals.h"


typedef struct {
    void *key;   // Pointer to the data
    void *val;   // Value corresponding to the key
    void *extra; // Extra data
} pair_t;

typedef struct {
    size_t nslots; // Number of slots allocated for in the table
    size_t npairs; // Number of pairs present in the table
    uint32_t *offsets;
    uint32_t *lengths;
    pair_t   *pairs;
} table_t;


/* # The `table_t` data part
 * 
 * The data part is fetched based on the `nslots` field. The offset and
 * length chunks are both `uint32_t` values and have NSLOTS values.
 * 
 * First comes the offset chunk. This is indexed using the hash value
 * of the key. The offset determines the start index for the values chunk.
 * 
 * Then comes the length chunk. This is also indexed using the key's hash
 * value. The offset determines how many values are stored after the offset,
 * INCLUDING the value at the offset.
 * 
 * After that comes the pairs chunk. This is structured based on the offsets,
 * with values coming directly after each other. A NULL is placed after the
 * last value of a hash. The first value is found by indexing with the offset
 * of the hash.
 * 
 */


// How much to multiply the number of slots by before calculating the next power of 2
#define SLOT_MULTIPLIER (4.0/3.0)

#define _OFFSETFIELD(table) ((uint32_t *)((char *)(table) + sizeof(table_t)))
#define _LENGTHFIELD(table) ((uint32_t *)((char *)(table) + sizeof(table_t) +  ((table)->nslots * sizeof(uint32_t))))
#define _PAIRSFIELD(table)  ((void     *)((char *)(table) + sizeof(table_t) + (((table)->nslots * sizeof(uint32_t)) * 2)))


// FNV-1a hashing algorithm
static _always_inline uint32_t fnv1a(const char *in, size_t size)
{
    uint32_t hash = 0x811C9DC5;

    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ in[i]) * 0x01000193;

    return hash;
}

static _always_inline uint32_t _hash_direct(void *key, uint32_t nslots)
{
    // SHR by 5 to leave out bits commonly aligned to 8, 16, or 32 boundaries
    return (uint32_t)(((uintptr_t)(key) >> 5) & (nslots - 1));
}

static _always_inline uint32_t _hash_pointer(void *ptr, size_t len, uint32_t nslots)
{
    return fnv1a((const char *)ptr, len) & (nslots - 1);
}

// Get the next power of 2 (returns 0 on N=0)
static _always_inline uint32_t next_power_of_2(uint32_t n)
{
    n--;
    n |= n >>  1;
    n |= n >>  2;
    n |= n >>  4;
    n |= n >>  8;
    n |= n >> 16;
    n++;

    return n;
}

#define _HASH_ANY_PAIR_T(pairs, i, nslots, string_keys) \
    !string_keys ? \
        _hash_direct((pairs)[i].key, nslots) \
    : \
        _hash_pointer((pairs)[i].key, (uintptr_t)pairs[i].extra, nslots)

// Create a hashtable.
// String keys must have KEY point towards the strings, and EXTRA towards the string length
static _always_inline table_t *hashtable_create(pair_t *_pairs, size_t npairs, bool string_keys)
{
    // Calculate the number of slots to allocate for
    size_t nslots = next_power_of_2(npairs * SLOT_MULTIPLIER);

    // Calculate the size of one pair, and that of all pairs
    size_t pairsize = npairs * sizeof(pair_t);

    // Calculate the size of the offset and length chunk
    size_t offsetsize = nslots * sizeof(uint32_t);
    size_t lengthsize = nslots * sizeof(uint32_t);

    // Allocate memory for the table itself, and the offset, length, and pairs chunks
    table_t *table = (table_t *)malloc(sizeof(table_t) + offsetsize + lengthsize + pairsize);

    if (table == NULL)
        return NULL;
    
    // Set the number of slots/pairs
    table->nslots = nslots;
    table->npairs = npairs;
    
    // Get pointers towards the table's data fields
    table->offsets = _OFFSETFIELD(table);
    table->lengths = _LENGTHFIELD(table);
    table->pairs = _PAIRSFIELD(table);
    
    // Set all length and pair fields to zero
    memset(table->lengths, 0, lengthsize + pairsize);

    // Initialize the lengths based on the hash values
    for (uint32_t i = 0; i < npairs; ++i)
        table->lengths[_HASH_ANY_PAIR_T(_pairs, i, nslots, string_keys)]++;
        
    // Initialize the offsets based on the length values
    table->offsets[0] = 0; // First offset is 0
    for (uint32_t i = 0; i < nslots - 1; ++i)
        table->offsets[i + 1] = table->offsets[i] + table->lengths[i];
    
    // Place the pairs into the table
    for (uint32_t i = 0; i < npairs; ++i)
    {
        size_t idx = table->offsets[_HASH_ANY_PAIR_T(_pairs, i, nslots, string_keys)];
        pair_t *slot = table->pairs + idx;

        while (slot->key != NULL)
            slot++;

        memcpy(slot, &_pairs[idx], sizeof(pair_t));
    }

    return table;
}

// Pull the pair object of KEY from the table
static _always_inline pair_t *hashtable_pull(table_t *table, void *key)
{
    // Get the hash value of the key
    uint32_t hash = _hash_direct(key, table->nslots);

    // Get the offset and length corresponding to the hash
    uint32_t offset = table->offsets[hash];
    uint32_t length = table->lengths[hash];

    // Iterate over all values belonging to this hash
    for (uint32_t i = 0; i < length; ++i)
    {
        // Get the pair stored on the current offset
        pair_t *pair = &table->pairs[offset + i];

        // Compare keys and return the pair object if it's a match
        if (pair->key == key)
            return pair;
    }

    // No match found
    return NULL;
}

// Pull the pair object of the string located on PTR from the table
static _always_inline pair_t *hashtable_string_pull(table_t *table, void *ptr, size_t len)
{
    // Get the hash value of the key
    uint32_t hash = _hash_pointer(ptr, len, table->nslots);

    // Get the offset and length corresponding to the hash
    uint32_t offset = table->offsets[hash];
    uint32_t length = table->lengths[hash];

    // Iterate over all values belonging to this hash
    for (uint32_t i = 0; i < length; ++i)
    {
        // Get the pair stored on the current offset
        pair_t *pair = table->pairs + offset + i;

        // Early continue if the lengths don't match
        if ((size_t)(pair->extra) != len)
            continue;

        // Compare the strings
        if (memcmp(pair->key, ptr, len) == 0)
            return pair;
    }

    // No match found
    return NULL;
}

// Get the internal hashtable pairs array (can be used for cleanup)
static _always_inline void *hashtable_get_pairs(table_t *table, size_t *npairs)
{
    *npairs = table->npairs;
    return table->pairs;
}

// Destroy the hashtable
static _always_inline void hashtable_destroy(table_t *table)
{
    free(table);
}


#endif // CMSGPACK_HASHTABLE_H