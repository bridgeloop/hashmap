#define _GNU_SOURCE

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include <stdio.h>

#include "../headers/hashmap.h"

#define XXH_INLINE_ALL
#include "headers/xxhash.h"

#define ENSURE(expr) if (expr != 0) { abort(); };

static void hashmap_entries_destroy(struct hashmap_entries *arr) {
	free(arr->nodes);
	arr->nodes = NULL;
	arr->n_nodes = 0;
	arr->base = 0;
	return;
}

static bool hashmap_entries_insert(
	struct hashmap_entries *arr,
	size_t idx,
	struct hashmap_entry *element
) {
	struct hashmap_entry **nodes = &(arr->nodes);
	size_t *base = &(arr->base), *n_nodes = &(arr->n_nodes);

	if (*nodes == NULL) {
		if (idx != 0) {
			abort();
		}
	} else {
		if (idx > *n_nodes) {
			abort();
		}
	}

	if (*nodes == NULL) {
		*nodes = malloc(sizeof(**nodes) * 2);
		if (*nodes == NULL) {
			return false;
		}
		*base = 1;
		*n_nodes = 1;

		(*nodes)[*base] = *element;

		return true;
	}

	if (*n_nodes > 1 && (*n_nodes & (*n_nodes - 1)) == 0) {
		if (*base != 0) {
			abort();
		}
		struct hashmap_entry *new_nodes = malloc(sizeof(**nodes) * (*n_nodes * 2));
		if (new_nodes == NULL) {
			return false;
		}
		*base = (*n_nodes + 1) / 2;
		memcpy(&(new_nodes[*base]), *nodes, idx * sizeof(**nodes));
		new_nodes[*base + idx] = *element;
		memcpy(&(new_nodes[*base + idx + 1]), &((*nodes)[idx]), (*n_nodes - idx) * sizeof(**nodes));
		free(*nodes);
		*nodes = new_nodes;
		*n_nodes += 1;
		return true;
	}

	#define move_elements_behind() memmove(&((*nodes)[*base - 1]), &((*nodes)[*base]), idx * sizeof(**nodes)); *base -= 1

	if (idx == 0 && *base != 0) {
		*base -= 1;
	} else if (*base > 0 && ((*n_nodes + *base) & (*n_nodes + *base - 1)) == 0) {
		move_elements_behind();
	} else if (idx != *n_nodes) {
		if (idx < *n_nodes / 2 && *base != 0) {
			move_elements_behind();
		} else {
			memmove(&((*nodes)[*base + idx + 1]), &((*nodes)[*base + idx]), (*n_nodes - idx) * sizeof(**nodes));
		}
	}

	#undef move_entries_behind

	(*nodes)[*base + idx] = *element;
	*n_nodes += 1;

	return true;
}

static void hashmap_entries_remove(struct hashmap_entries *arr, size_t idx) {
	struct hashmap_entry **nodes = &(arr->nodes);
	size_t *base = &(arr->base), *n_nodes = &(arr->n_nodes);

	if (*nodes == NULL) {
		abort();
	}

	if (idx < *n_nodes / 2) {
		// move elements at the start of the array forward by one position
		memmove(&((*nodes)[*base]), &((*nodes)[*base + 1]), idx * sizeof(**nodes));
		*base += 1;
	} else {
		// move the elements at the end of the array backward by one position
		memmove(&((*nodes)[idx + 1]), &((*nodes)[idx]), (*n_nodes - idx) * sizeof(**nodes));
	}

	*n_nodes -= 1;

	if (*n_nodes > 1 && (*n_nodes & (*n_nodes - 1)) == 0) {
		memmove(nodes, &((*nodes)[*base]), sizeof(**nodes) * *n_nodes);
		struct hashmap_entry *new_nodes = realloc(*nodes, sizeof(**nodes) * *n_nodes);
		*base = 0;
		if (new_nodes != NULL) {
			*nodes = new_nodes;
		}
	} else if (*n_nodes == 0) {
		hashmap_entries_destroy(arr);
	}

	return;
}

static inline struct hashmap_entry *hashmap_entries_get(struct hashmap_entries *arr, size_t *sz) {
	if (arr->nodes == NULL) {
		if (sz != NULL) {
			*sz = 0;
		}
		return NULL;
	}
	if (sz != NULL) {
		*sz = arr->n_nodes;
	}
	return &(arr->nodes[arr->base]);
}

static inline size_t hashmap_hash(const unsigned char *key, size_t key_sz) {
	return XXH3_64bits(key, key_sz);
}

static inline pthread_mutex_t *hashmap_mutexes(struct hashmap *hashmap) {
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	pthread_mutex_t *mutexes = (pthread_mutex_t *)&(buckets[hashmap->n_buckets]);
	return mutexes;
}

static void hashmap_delete_entries(struct hashmap *hashmap) {
	hashmap_drop_handler drop_handler = hashmap->drop_handler;
	while (hashmap->bucket_with_entries != NULL) {
		struct hashmap_bucket *bucket = hashmap->bucket_with_entries;
		hashmap->bucket_with_entries = bucket->next;

		bucket->prev_next = NULL;
		bucket->next = NULL;

		size_t n_entries;
		struct hashmap_entry *entries = hashmap_entries_get(&(bucket->entries), &(n_entries));
		if (drop_handler != NULL) {
			for (size_t idx = 0; idx < n_entries; ++idx) {
				drop_handler(entries[idx].inner->value, hashmap_drop_delete);
				free(entries[idx].inner);
			}
		} else {
			for (size_t idx = 0; idx < n_entries; ++idx) {
				free(entries[idx].inner);
			}
		}
		hashmap_entries_destroy(&(bucket->entries));
	}
	return;
}

static void hashmap_destroy(struct hashmap *hashmap) {
	pthread_mutex_t *mutexes = hashmap_mutexes(hashmap);
	for (size_t idx = 0; idx < hashmap->n_divisions; ++idx) {
		if (pthread_mutex_destroy(&(mutexes[idx])) != 0) {
			fputs("programming error: tried to delete a hashmap's entries while holding one of its keys\n", stderr);
			abort();
		}
	}
	ENSURE(pthread_mutex_destroy(&(hashmap->meta_mutex)));
	hashmap_delete_entries(hashmap);
	free(hashmap);
	return;
}

struct hashmap *hashmap_copy_ref(struct hashmap *hashmap) {
	pthread_mutex_t *mutex = &(hashmap->meta_mutex);
	ENSURE(pthread_mutex_lock(mutex));
	if (hashmap->ref_count == (SIZE_MAX - 1)) {
		hashmap = NULL;
	} else {
		hashmap->ref_count += 1;
	}
	ENSURE(pthread_mutex_unlock(mutex));
	return hashmap;
}
struct hashmap *hashmap_move_ref(struct hashmap **src) {
	struct hashmap *hashmap = *src;
	*src = NULL;
	return hashmap;
}
void hashmap_destroy_ref(struct hashmap **src) {
	struct hashmap *hashmap = *src;
	*src = NULL;
	ENSURE(pthread_mutex_lock(&(hashmap->meta_mutex)));
	bool destroy = --hashmap->ref_count == 0;
	ENSURE(pthread_mutex_unlock(&(hashmap->meta_mutex)));

	if (destroy) {
		hashmap_destroy(hashmap);
	}

	return;
}

struct hashmap *hashmap_create(
	size_t n_buckets,
	size_t n_divisions,
	hashmap_drop_handler drop_handler
) {
	if (n_buckets == 0 || n_divisions == 0) {
		return NULL;
	}
	if (n_divisions > n_buckets) {
		n_divisions = n_buckets;
	}
	struct hashmap *hashmap = malloc(
		sizeof(struct hashmap) +
		(sizeof(struct hashmap_bucket) * n_buckets) +
		(sizeof(pthread_mutex_t) * n_divisions)
	);
	if (hashmap == NULL) {
		return NULL;
	}

	bool attr_s = false;

	pthread_mutex_t mutex_initializer = PTHREAD_MUTEX_INITIALIZER;
	memcpy(&(hashmap->meta_mutex), &(mutex_initializer), sizeof(pthread_mutex_t));

	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	pthread_mutex_t *mutexes = (pthread_mutex_t *)&(buckets[n_buckets]);

	hashmap->drop_handler = drop_handler;
	hashmap->ref_count = 1;

	hashmap->n_buckets = n_buckets;
	hashmap->n_divisions = 0;

	hashmap->bucket_with_entries = NULL;

	for (size_t idx = 0; idx < n_buckets; ++idx) {
		buckets[idx].next = NULL;
		buckets[idx].prev_next = NULL;
		hashmap_entries_destroy(&(buckets[idx].entries));
	}

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&(attr)) != 0) {
		goto err;
	}
	attr_s = true;
	pthread_mutexattr_settype(&(attr), PTHREAD_MUTEX_RECURSIVE);

	if (pthread_mutex_init(&(hashmap->meta_mutex), NULL) != 0) {
		goto err;
	}
	for (; hashmap->n_divisions < n_divisions; ++(hashmap->n_divisions)) {
		pthread_mutex_t *mutex = &(mutexes[hashmap->n_divisions]);
		if (pthread_mutex_init(mutex, &(attr)) != 0) {
			goto err;
		}
	}
	goto out;

	err:;
	hashmap_destroy(hashmap);
	hashmap = NULL;
	out:;
	if (attr_s) {
		ENSURE(pthread_mutexattr_destroy(&(attr)));
	}
	return hashmap;
}

static pthread_mutex_t *hashmap_division_mutex_for_bucket(
	struct hashmap *hashmap,
	size_t bucket_id
) {
	size_t n_divisions = hashmap->n_divisions;
	size_t buckets_per_division = hashmap->n_buckets / n_divisions;
	pthread_mutex_t *mutexes = hashmap_mutexes(hashmap);
	size_t division = bucket_id / buckets_per_division;
	if (division == n_divisions) {
		assert(n_divisions != 0);
		division -= 1;
	}
	return &(mutexes[division]);
}

#ifndef NDEBUG
static void validate_bucket(struct hashmap *hashmap, struct hashmap_bucket *bucket) {
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	assert(bucket >= buckets);
	assert(bucket < &(buckets[hashmap->n_buckets]));
	assert(((uintptr_t)bucket - (uintptr_t)buckets) % sizeof(struct hashmap_bucket) == 0);
}
#else
#define validate_bucket(x, y) ;
#endif

static pthread_mutex_t *hashmap_key_locked_mutex(struct hashmap *hashmap, struct hashmap_key *key) {
	struct hashmap_bucket *bucket = key->bucket;
	if (bucket == NULL) {
		return NULL;
	}
	validate_bucket(hashmap, bucket);
	unsigned char *buckets = hashmap->buf;
	size_t locked_bucket_id = ((unsigned char *)bucket - buckets) / sizeof(struct hashmap_bucket);
	return hashmap_division_mutex_for_bucket(hashmap, locked_bucket_id);
}

void hashmap_key_initialise(struct hashmap_key *key) {
	key->key = NULL;
	key->key_sz = 1;
	key->bucket = NULL;
	return;
}

void hashmap_key_obtain(
	struct hashmap *hashmap,
	struct hashmap_key *hmap_key,
	const unsigned char *key,
	size_t key_sz
) {
	if (key == NULL && key_sz != 0) {
		abort();
	}
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	size_t hash = hashmap_hash(key, key_sz);
	size_t bucket_id = hash % hashmap->n_buckets;

	pthread_mutex_t *division_mutex = hashmap_division_mutex_for_bucket(hashmap, bucket_id);
	pthread_mutex_t *locked_mutex = hashmap_key_locked_mutex(hashmap, hmap_key);

	if (division_mutex != locked_mutex) {
		if (locked_mutex != NULL) {
			ENSURE(pthread_mutex_unlock(locked_mutex));
		}
		ENSURE(pthread_mutex_lock(division_mutex));
	}
	hmap_key->key = key;
	hmap_key->key_sz = key_sz;
	hmap_key->hash = hash;
	hmap_key->bucket = &(buckets[bucket_id]);
	return;
}
void hashmap_key_release(struct hashmap *hashmap, struct hashmap_key *key, bool hold_lock) {
	if (key->bucket == NULL) {
		return;
	}
	// invalidate key
	key->key = NULL;
	key->key_sz = 1 /* arbitrary non-zero value */;
	if (!hold_lock) {
		ENSURE(pthread_mutex_unlock(
			hashmap_key_locked_mutex(hashmap, key)
		));
		key->bucket = NULL;
	}
	return;
}

static bool hashmap_entry_find(struct hashmap_key *key, struct hashmap_entry **out_entry) {
	/*
	 * shits are (unstable) sorted by hash, then by key_sz, and then by the byte values in the key itself
	 * e.g.
	 * +---------+---------+---------+
	 * |hash:0   |hash:0   |hash:1   |
	 * |key_sz:12|key_sz:16|key_sz:11|
	 * |inner:...|inner:...|inner:...|
	 * +---------+---------+---------+
	 */
	size_t hash = key->hash;
	size_t key_sz = key->key_sz;

	// find stage 1: binary search for hash
	size_t n_entries;
	struct hashmap_entry *entries = hashmap_entries_get(&(key->bucket->entries), &(n_entries));
	struct hashmap_entry *entries_subset = entries;
	struct hashmap_entry *after_entries = &(entries[n_entries]);
	// do not call this function if there are no entries in the bucket
	assert(entries != NULL);
	size_t len = n_entries;
	assert(len != 0);
	struct hashmap_entry *entry;
	for (;;) {
		size_t idx = len / 2;
		size_t entry_hash = entries_subset[idx].hash;
		if (hash < entry_hash) {
			len /= 2;
		} else {
			if (hash == entry_hash) {
				entry = &(entries_subset[idx]);
				break;
			}
			entries_subset = &(entries_subset[idx + 1]);
			len = (len - 1) / 2;
		}
		if (len == 0) {
			*out_entry = entries_subset;
			return false;
		}
	}

	int memcmp_result;

	// find stage 2: look for matching keys
	#define entry_memcmp(entry) \
	if ((memcmp_result = memcmp(key->key, entry->inner->key, key_sz)) == 0) { \
		*out_entry = entry; \
		return true; \
	}

	if (key_sz == entry->key_sz) {
		entry_memcmp(entry);
		if (memcmp_result > 0) {
			for (
				entry += 1;
				entry < after_entries && key_sz == entry->key_sz && hash == entry->hash;
				++entry
			) {
				entry_memcmp(entry);
				if (memcmp_result < 0) {
					break;
				}
			}
		} else if (entry != entries) {
			for (
				entry -= 1;
				key_sz == entry->key_sz && hash == entry->hash;
				--entry
			) {
				entry_memcmp(entry);
				if (memcmp_result > 0) {
					break;
				}
				if (entry == entries) {
					goto err;
				}
			}
			entry += 1;
		}
		err:;
		*out_entry = entry;
		return false;
	}

	#define check_sentinel() if (entry == sentinel) { goto err; }

	struct hashmap_entry *sentinel;
	if (key_sz < entry->key_sz) {
		sentinel = entries;
		check_sentinel();

		entry -= 1;
		for (;;) {
			if (entry->hash != hash || entry->key_sz < key_sz) {
				entry += 1;
				goto err;
			}
			if (entry->key_sz == key_sz) {
				break;
			}

			check_sentinel();
			entry -= 1;
		}

		do {
			entry_memcmp(entry);
			if (memcmp_result > 0) {
				break;
			}
			check_sentinel();
			entry -= 1;
		} while (entry->key_sz == key_sz && entry->hash == hash);

		entry += 1;
		goto err;
	} else {
		sentinel = after_entries - 1;
		check_sentinel();

		entry += 1;
		for (;;) {
			if (entry->hash != hash || entry->key_sz > key_sz) {
				goto err;
			}
			if (entry->key_sz == key_sz) {
				break;
			}

			check_sentinel();
			entry += 1;
		}

		do {
			entry_memcmp(entry);
			if (memcmp_result < 0) {
				break;
			}
			check_sentinel();
			entry += 1;
		} while (entry->key_sz == key_sz && entry->hash == hash);

		goto err;
	}

	#undef entry_memcmp
	#undef check_sentinel
}

bool hashmap_get(struct hashmap *hashmap, struct hashmap_key *key, void **value) {
	if (
		key->bucket == NULL ||
		(key->key == NULL && key->key_sz != 0)
	) {
		return false;
	}

	validate_bucket(hashmap, key->bucket);
	if (hashmap_entries_get(&(key->bucket->entries), NULL) == NULL) {
		return false;
	}

	struct hashmap_entry *entry;
	if (!hashmap_entry_find(key, &(entry))) {
		return false;
	}

	*value = entry->inner->value;
	return true;
}

static inline struct hashmap_entry_inner *hashmap_alloc_entry_inner(struct hashmap_key *key, void *value) {
	struct hashmap_entry_inner *inner = malloc(
		sizeof(struct hashmap_entry_inner) +
		key->key_sz
	);
	if (inner == NULL) {
		return NULL;
	}
	memcpy(inner->key, key->key, key->key_sz);
	inner->value = value;
	return inner;
}
bool hashmap_set(struct hashmap *hashmap, struct hashmap_key *key, void *value) {
	if (
		key->bucket == NULL /* no lock obtained */ ||
		(key->key == NULL && key->key_sz != 0) /* key dropped */
	) {
		return false;
	}
	validate_bucket(hashmap, key->bucket);
	struct hashmap_bucket *bucket = key->bucket;

	struct hashmap_entry *entries = hashmap_entries_get(&(bucket->entries), NULL);

	size_t idx = 0;

	#define add_entry() \
	struct hashmap_entry entry = { \
		.hash = key->hash, \
		.key_sz = key->key_sz, \
		.inner = hashmap_alloc_entry_inner(key, value), \
	}; \
	if (entry.inner == NULL) { \
		return false; \
	} \
	if (!hashmap_entries_insert(&(bucket->entries), idx, &(entry))) { \
		free(entry.inner); \
		return false; \
	}

	if (entries == NULL) {
		add_entry();
		ENSURE(pthread_mutex_lock(&(hashmap->meta_mutex)));
		struct hashmap_bucket **bucket_with_entries = &(hashmap->bucket_with_entries);
		bucket->prev_next = bucket_with_entries;
		if (bucket->next != NULL) {
			bucket->next->prev_next = &(bucket->next);
		}
		bucket->next = (*bucket_with_entries);
		(*bucket_with_entries) = bucket;
		ENSURE(pthread_mutex_unlock(&(hashmap->meta_mutex)));
		return true;
	}

	{
		struct hashmap_entry *entry;
		if (hashmap_entry_find(key, &(entry))) {
			if (hashmap->drop_handler != NULL) {
				hashmap->drop_handler(entry->inner->value, hashmap_drop_set);
			}
			entry->inner->value = value;
			return true;
		}
		idx = entry - entries;
	}

	add_entry();

	return true;

	#undef add_entry
}
bool hashmap_delete(struct hashmap *hashmap, struct hashmap_key *key) {
	if (
		key->bucket == NULL ||
		(key->key == NULL && key->key_sz != 0)
	) {
		return false;
	}

	struct hashmap_bucket *bucket = key->bucket;
	validate_bucket(hashmap, bucket);

	size_t n_entries;
	struct hashmap_entry *entries = hashmap_entries_get(&(bucket->entries), &(n_entries));
	if (entries == NULL) {
		return false;
	}

	struct hashmap_entry *entry;
	if (!hashmap_entry_find(key, &(entry))) {
		return false;
	}

	if (hashmap->drop_handler != NULL) {
		hashmap->drop_handler(entry->inner->value, hashmap_drop_delete);
	}
	free(entry->inner);

	hashmap_entries_remove(&(bucket->entries), (size_t)(entry - entries));

	if (n_entries == 1) {
		ENSURE(pthread_mutex_lock(&(hashmap->meta_mutex)));
		struct hashmap_bucket *next_bucket = bucket->next;
		(*bucket->prev_next) = next_bucket;
		if (next_bucket != NULL) {
			next_bucket->prev_next = bucket->prev_next;
		}
		bucket->next = NULL;
		bucket->prev_next = NULL;
		ENSURE(pthread_mutex_unlock(&(hashmap->meta_mutex)));
	}

	return true;
}

#ifdef HASHMAP_STATIC_ENABLED
void hashmap_static_key_obtain(
	void *static_hashmap,
	struct hashmap_key *hmap_key,
	const unsigned char *key,
	size_t key_sz
) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);
	return hashmap_key_obtain(hashmap, hmap_key, key, key_sz);
}
void hashmap_static_key_release(void *static_hashmap, struct hashmap_key *key, bool hold_lock) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);
	return hashmap_key_release(hashmap, key, hold_lock);
}

void hashmap_static_delete_entries(void *static_hashmap) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);

	pthread_mutex_t *mutexes = hashmap_mutexes(hashmap);

	for (size_t idx = 0; idx < hashmap->n_divisions; ++idx) {
		ENSURE(pthread_mutex_lock(&(mutexes[idx])));
	}
	ENSURE(pthread_mutex_lock(&(hashmap->meta_mutex)));
	hashmap_delete_entries(hashmap);
	ENSURE(pthread_mutex_unlock(&(hashmap->meta_mutex)));
	for (size_t idx = 0; idx < hashmap->n_divisions; ++idx) {
		ENSURE(pthread_mutex_unlock(&(mutexes[idx])));
	}
	return;
}

bool hashmap_static_get(void *static_hashmap, struct hashmap_key *key, void **value) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);
	return hashmap_get(hashmap, key, value);
}
bool hashmap_static_set(void *static_hashmap, struct hashmap_key *key, void *value) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);
	return hashmap_set(hashmap, key, value);
}
bool hashmap_static_delete(void *static_hashmap, struct hashmap_key *key) {
	struct hashmap *hashmap = static_hashmap;
	assert(hashmap->ref_count == SIZE_MAX);
	return hashmap_delete(hashmap, key);
}
#endif

#ifdef HASHMAP_MAIN
#include <time.h>
int main(void) {
	struct hashmap *hashmap = hashmap_create(183, 1, NULL);
	struct hashmap_key key = HASHMAP_KEY_INITIALIZER;
	puts("test: hashmap_set");
	clock_t time = clock();
	for (size_t it = 0; it < 0x50000; ++it) {
		hashmap_key_obtain(hashmap, &(key), (void *)&(it), sizeof(size_t));
		if (!hashmap_set(hashmap, &(key), (void *)it)) {
			printf("hashmap_set failed on iter %zu!\n", it);
			return 1;
		}
	}
	printf("test success in %lf\n", (double)(clock() - time) / (double)CLOCKS_PER_SEC);
	puts("test: hashmap_get");
	time = clock();
	for (size_t it = 0; it < 0x50000; ++it) {
		hashmap_key_obtain(hashmap, &(key), (void *)&(it), sizeof(size_t));
		void *val;
		bool success = hashmap_get(hashmap, &(key), &(val));
		if (!success || val != (void *)it) {
			printf("hashmap_get failed on iter %zu!\n", it);
			return 1;
		}
	}
	printf("test success in %lf\n", (double)(clock() - time) / (double)CLOCKS_PER_SEC);
	hashmap_key_release(hashmap, &(key), false);
	puts("test: hashmap_destroy_ref");
	time = clock();
	hashmap_destroy_ref(&(hashmap));
	printf("test success in %lf\n", (double)(clock() - time) / (double)CLOCKS_PER_SEC);

	return 0;
}
#endif
