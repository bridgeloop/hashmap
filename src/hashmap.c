#define _GNU_SOURCE

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include <stdio.h>

#include "../headers/hashmap.h"

#define ENSURE(expr) if (expr != 0) { abort(); };

static unsigned int hashmap_hash(const unsigned char *key, size_t key_sz) {
	unsigned int hash = 0;
	while (key_sz--) {
		hash += *(key++);
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

static void hashmap_entry_destroy(
	struct hashmap *hashmap,
	struct hashmap_bucket *bucket,
	struct hashmap_entry **next,
	enum hashmap_drop_mode drop_mode
) {
	struct hashmap_entry *entry = *next;
	*next = entry->next;
	if (hashmap->drop_handler != NULL) {
		hashmap->drop_handler(entry->value, drop_mode);
	}
	free(entry);
	if (bucket->entries == NULL) {
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
	return;
}

static void hashmap_destroy(struct hashmap *hashmap) {
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	pthread_mutex_t *mutexes = (pthread_mutex_t *)&(buckets[hashmap->n_buckets]);
	for (size_t idx = 0; idx < hashmap->n_divisions; ++idx) {
		if (pthread_mutex_destroy(&(mutexes[idx])) != 0) {
			fputs("programming error: tried to destroy a hashmap while holding one of its keys\n", stderr);
			abort();
		}
	}
	struct hashmap_bucket **bucket_with_entries = &(hashmap->bucket_with_entries);
	while ((*bucket_with_entries) != NULL) {
		hashmap_entry_destroy(
			hashmap,
			(*bucket_with_entries),
			&((*bucket_with_entries)->entries),
			hashmap_drop_delete
		);
	}
	ENSURE(pthread_mutex_destroy(&(hashmap->meta_mutex)));
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
	void (*drop_handler)(void *value, enum hashmap_drop_mode drop_mode)
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
		buckets[idx].entries = NULL;
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
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	pthread_mutex_t *mutexes = (pthread_mutex_t *)&(buckets[hashmap->n_buckets]);
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
	struct hashmap_bucket *buckets = (struct hashmap_bucket *)hashmap->buf;
	size_t bucket_id = hashmap_hash(key, key_sz) % hashmap->n_buckets;

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

static struct hashmap_entry **hashmap_entry_find(struct hashmap_key *key) {
	struct hashmap_entry **next = &(key->bucket->entries);
	while ((*next) != NULL) {
		if (
			(*next)->key_sz == key->key_sz &&
			memcmp((*next)->key, key->key, key->key_sz) == 0
		) {
			return next;
		}
		next = &((*next)->next);
	}
	return NULL;
}

bool hashmap_get(struct hashmap *hashmap, struct hashmap_key *key, void **value) {
	if (
		key->bucket == NULL ||
		(key->key == NULL && key->key_sz != 0)
	) {
		return false;
	}
	validate_bucket(hashmap, key->bucket);
	struct hashmap_entry **next = hashmap_entry_find(key);
	if (next == NULL) {
		return false;
	}
	*value = (*next)->value;
	return true;
}
bool hashmap_set(struct hashmap *hashmap, struct hashmap_key *key, void *value) {
	if (
		key->bucket == NULL /* no lock obtained */ ||
		(key->key == NULL && key->key_sz != 0) /* key dropped */
	) {
		return false;
	}
	validate_bucket(hashmap, key->bucket);
	struct hashmap_entry *entry = NULL;
	struct hashmap_entry **next = hashmap_entry_find(key);
	if (next != NULL) {
		entry = *next;
	}
	struct hashmap_bucket *bucket = key->bucket;
	if (entry == NULL) {
		size_t key_sz = key->key_sz;
		entry = malloc(
			sizeof(struct hashmap_entry) +
			key_sz
		);
		if (entry == NULL) {
			return false;
		}
		memcpy(entry->key, key->key, key_sz);
		entry->key_sz = key_sz;
		if (bucket->entries == NULL) {
			ENSURE(pthread_mutex_lock(&(hashmap->meta_mutex)));
			struct hashmap_bucket **bucket_with_entries = &(hashmap->bucket_with_entries);
			bucket->prev_next = bucket_with_entries;
			if (bucket->next != NULL) {
				bucket->next->prev_next = &(bucket->next);
			}
			bucket->next = (*bucket_with_entries);
			(*bucket_with_entries) = bucket;
			ENSURE(pthread_mutex_unlock(&(hashmap->meta_mutex)));
		}
		entry->next = bucket->entries;
		bucket->entries = entry;
	} else {
		if (hashmap->drop_handler != NULL && entry->value != value) {
			hashmap->drop_handler(entry->value, hashmap_drop_set);
		}
	}
	entry->value = value;
	return true;
}
bool hashmap_delete(struct hashmap *hashmap, struct hashmap_key *key) {
	if (
		key->bucket == NULL ||
		(key->key == NULL && key->key_sz != 0)
	) {
		return false;
	}
	validate_bucket(hashmap, key->bucket);
	struct hashmap_entry **next = hashmap_entry_find(key);
	if (next == NULL) {
		return false;
	}
	hashmap_entry_destroy(
		hashmap,
		key->bucket,
		next,
		hashmap_drop_delete
	);
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
#include <stdio.h>
int main(void) {
	void *ptr = "hi";
	void *val;

	struct hashmap *hashmap = hashmap_create(183, 1, NULL);
	struct hashmap_key key = HASHMAP_KEY_INITIALIZER;
	hashmap_key_obtain(hashmap, &(key), "x", 1);
	hashmap_set(hashmap, &(key), ptr);
	hashmap_get(hashmap, &(key), &(val));
	if (val != ptr) {
		return 1;
	}
	hashmap_key_release(hashmap, &(key), false);
	hashmap_destroy_ref(&(hashmap));

	#ifdef HASHMAP_STATIC_ENABLED
	typedef hashmap_create_static_type(183, 1) static_hashmap;
	static static_hashmap test = hashmap_create_static_value(static_hashmap, NULL);
	hashmap_static_key_obtain(&(test), &(key), "x", 1);
	hashmap_static_set(&(test), &(key), ptr);
	hashmap_static_get(&(test), &(key), &(val));
	if (val != ptr) {
		return 1;
	}
	hashmap_static_key_release(&(test), &(key), false);
	#endif

	return 0;
}
#endif
