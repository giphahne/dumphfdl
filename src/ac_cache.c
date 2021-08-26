/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <time.h>                           // time_t
#include <libacars/hash.h>                  // la_hash_*
#include "util.h"                           // NEW, debug_print
#include "cache.h"                          // cache_*
#include "ac_cache.h"                       // ac_cache

// This object is used to cache mappings between aircraft ID numbers (extracted
// from PDU headers) and their ICAO hex addresses. fwd_cache is used for
// forward lookups (eg. to replace aircraft ID with its ICAO code in formatted
// output), while inv_cache is used for inverse lookups, which are necessary
// for deletion of entries from the first map when a Logoff confirm LPDU is
// received (it contains the destination aircraft's ICAO code, but is always
// sent to the broadcast Aircraft ID (255), so an inverse lookup is necessary
// to locate the entry in fwd_cache for cleaning).

struct ac_cache {
	struct cache *fwd_cache;
	struct cache *inv_cache;
};

// Hash key definitions.
// Both hashes are keyed with channel frequency and aircraft identifier:
// - forward hash: by ID
// - inverse hash: by ICAO address
struct ac_cache_fwd_key {
	int32_t freq;
	uint8_t id;
};
struct ac_cache_inv_key {
	int32_t freq;
	uint32_t icao_address;
};

// Inverse mapping cache entry definition.
// This one is used for internal purposes only, so it's defined here
// rather than in ac_cache.h.
struct ac_cache_inv_entry {
	uint8_t id;
};

#define AC_CACHE_TTL 14400L
#define AC_CACHE_EXPIRATION_INTERVAL 309L

/******************************
 * Forward declarations
 ******************************/

static struct cache_vtable ac_cache_fwd_vtable;
static struct cache_vtable ac_cache_inv_vtable;
static void ac_cache_fwd_entry_create(ac_cache const *cache, int32_t freq,
		uint8_t id, uint32_t icao_address, time_t created_time);
static void ac_cache_inv_entry_create(ac_cache const *cache, int32_t freq,
		uint8_t id, uint32_t icao_address, time_t created_time);

/******************************
 * Public methods
 ******************************/

ac_cache *ac_cache_create(void) {
	NEW(ac_cache, cache);
	cache->fwd_cache = cache_create(&ac_cache_fwd_vtable, AC_CACHE_TTL, AC_CACHE_EXPIRATION_INTERVAL);
	cache->inv_cache = cache_create(&ac_cache_inv_vtable, AC_CACHE_TTL, AC_CACHE_EXPIRATION_INTERVAL);
	return cache;
}

void ac_cache_entry_create(ac_cache const *cache, int32_t freq,
		uint8_t id, uint32_t icao_address) {
	ASSERT(cache != NULL);

	time_t now = time(NULL);
	ac_cache_fwd_entry_create(cache, freq, id, icao_address, now);
	ac_cache_inv_entry_create(cache, freq, id, icao_address, now);

	debug_print(D_CACHE, "new entry: %hhu@%d: %06X\n",
			id, freq, icao_address);
}

bool ac_cache_entry_delete(ac_cache const *cache, int32_t freq,
		uint32_t icao_address) {
	ASSERT(cache != NULL);

	bool result = false;
	struct ac_cache_inv_key inv_key = { .freq = freq, .icao_address = icao_address };
	struct ac_cache_inv_entry *e = cache_entry_lookup(cache->inv_cache, &inv_key);
	if(e != NULL) {
		struct ac_cache_fwd_key fwd_key = { .freq = freq, .id = e->id };
		result  = cache_entry_delete(cache->inv_cache, &inv_key);
		result |= cache_entry_delete(cache->fwd_cache, &fwd_key);
		if(result) {
			debug_print(D_CACHE, "entry deleted: %06X@%d: %hhu\n", icao_address, freq, e->id);
		} else {
			debug_print(D_CACHE, "entry deletion failed: %06X@%d: %hhu\n", icao_address, freq, e->id);
		}
	} else {
		debug_print(D_CACHE, "entry not deleted: %06X@%d: not found\n", icao_address, freq);
	}
	return result;
}

struct ac_cache_entry *ac_cache_entry_lookup(ac_cache *cache, int32_t freq, uint8_t id) {
	ASSERT(cache != NULL);

	struct ac_cache_fwd_key fwd_key = { .freq = freq, .id = id };
	struct ac_cache_entry *e = cache_entry_lookup(cache->fwd_cache, &fwd_key);
	if(e != NULL) {
		debug_print(D_CACHE, "%hhu@%d: %06X\n", id, freq, e->icao_address);
	} else {
		debug_print(D_CACHE, "%hhu@%d: not found\n", id, freq);
	}
	return e;
}

void ac_cache_destroy(ac_cache *cache) {
	if(cache != NULL) {
		cache_destroy(cache->fwd_cache);
		cache_destroy(cache->inv_cache);
		XFREE(cache);
	}
}

/****************************************
 * Private variables and methods
 ****************************************/

static uint32_t ac_cache_fwd_key_hash(void const *key);
static bool ac_cache_fwd_key_compare(void const *key1, void const *key2);
static void ac_cache_fwd_entry_destroy(void *data);
static struct cache_vtable ac_cache_fwd_vtable = {
	.cache_key_hash = ac_cache_fwd_key_hash,
	.cache_key_compare = ac_cache_fwd_key_compare,
	.cache_key_destroy = la_simple_free,
	.cache_entry_data_destroy = ac_cache_fwd_entry_destroy
};

static uint32_t ac_cache_inv_key_hash(void const *key);
static bool ac_cache_inv_key_compare(void const *key1, void const *key2);
static struct cache_vtable ac_cache_inv_vtable = {
	.cache_key_hash = ac_cache_inv_key_hash,
	.cache_key_compare = ac_cache_inv_key_compare,
	.cache_key_destroy = la_simple_free,
	.cache_entry_data_destroy = la_simple_free
};

static uint32_t ac_cache_fwd_key_hash(void const *key) {
	ASSERT(key);
	struct ac_cache_fwd_key const *k = key;
	return k->freq + k->id;
}

static bool ac_cache_fwd_key_compare(void const *key1, void const *key2) {
	ASSERT(key1);
	ASSERT(key2);
	struct ac_cache_fwd_key const *k1 = key1;
	struct ac_cache_fwd_key const *k2 = key2;
	return (k1->freq == k2->freq && k1->id == k2->id);
}

static void ac_cache_fwd_entry_destroy(void *data) {
	if(data != NULL) {
		struct ac_cache_entry *e = data;
		if(e->callsign) {
			XFREE(e->callsign);
		}
		XFREE(e);
	}
}

static uint32_t ac_cache_inv_key_hash(void const *key) {
	struct ac_cache_inv_key const *k = key;
	return k->freq + k->icao_address;
}

static bool ac_cache_inv_key_compare(void const *key1, void const *key2) {
	ASSERT(key1);
	ASSERT(key2);
	struct ac_cache_inv_key const *k1 = key1;
	struct ac_cache_inv_key const *k2 = key1;
	return (k1->freq == k2->freq && k1->icao_address == k2->icao_address);
}

static void ac_cache_fwd_entry_create(ac_cache const *cache, int32_t freq,
		uint8_t id, uint32_t icao_address, time_t created_time) {
	ASSERT(cache != NULL);
	NEW(struct ac_cache_entry, fwd_entry);
	fwd_entry->icao_address = icao_address;
	NEW(struct ac_cache_fwd_key, fwd_key);
	fwd_key->freq = freq;
	fwd_key->id = id;
	cache_entry_create(cache->fwd_cache, fwd_key, fwd_entry, created_time);
}

static void ac_cache_inv_entry_create(ac_cache const *cache, int32_t freq,
		uint8_t id, uint32_t icao_address, time_t created_time) {
	ASSERT(cache != NULL);

	NEW(struct ac_cache_inv_entry, inv_entry);
	inv_entry->id = id;
	NEW(struct ac_cache_inv_key, inv_key);
	inv_key->freq = freq;
	inv_key->icao_address = icao_address;
	cache_entry_create(cache->inv_cache, inv_key, inv_entry, created_time);
}
