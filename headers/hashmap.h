#ifndef HASHMAP_H
#define HASHMAP_H

#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

enum hashmap_drop_mode {
	hashmap_drop_set,
	hashmap_drop_delete,
};

typedef void (*hashmap_drop_handler)(void *value, enum hashmap_drop_mode drop_mode);

struct hashmap_entries {
  struct hashmap_entry *nodes;
  size_t base, n_nodes;
};

struct hashmap_bucket {
	struct hashmap_entries entries;
	struct hashmap_bucket **prev_next;
	struct hashmap_bucket *next;
};

struct hashmap_key {
	const unsigned char *key;
	size_t key_sz;
	size_t hash;
	struct hashmap_bucket *bucket;
};
#define HASHMAP_KEY_INITIALIZER { .bucket = NULL }

struct hashmap_entry {
	size_t hash;
	size_t key_sz;
	struct hashmap_entry_inner *inner;
};
struct hashmap_entry_inner {
	void *value;
	unsigned char key[];
};

#define __hashmap_header \
	hashmap_drop_handler drop_handler; \
	size_t n_divisions; \
	size_t n_buckets; \
	\
	pthread_mutex_t meta_mutex; \
	struct hashmap_bucket *bucket_with_entries; \
	size_t ref_count

struct _hashmap_header {
	__hashmap_header;
};

#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define HASHMAP_STATIC_ENABLED
#define hashmap_create_static_type(arg_buckets, arg_divisions) struct __attribute__((packed)) { \
	struct _hashmap_header header; \
	struct hashmap_bucket buckets[arg_buckets]; \
	pthread_mutex_t mutexes[arg_divisions]; \
}
#define hashmap_create_static_value(type, arg_drop_handler) { \
	.header = { \
		.drop_handler = arg_drop_handler, \
		.n_divisions = sizeof(((type *)NULL)->mutexes) / sizeof(pthread_mutex_t), \
		.n_buckets = sizeof(((type *)NULL)->buckets) / sizeof(struct hashmap_bucket), \
		\
		.meta_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, \
		.bucket_with_entries = NULL, \
		.ref_count = SIZE_MAX, \
	}, \
	.buckets = { [0 ... sizeof(((type *)NULL)->buckets) / sizeof(struct hashmap_bucket) - 1] = { \
		.entries = { \
			.nodes = NULL, \
			.n_nodes = 0, \
			.base = 0, \
		}, \
		.prev_next = NULL, \
		.next = NULL, \
	}, }, \
	.mutexes = { [0 ... sizeof(((type *)NULL)->mutexes) / sizeof(pthread_mutex_t) - 1] = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, } \
}

void hashmap_static_key_obtain(
	void *static_hashmap,
	struct hashmap_key *hmap_key,
	const unsigned char *key,
	size_t key_sz
);
void hashmap_static_key_release(void *static_hashmap, struct hashmap_key *key, bool hold_lock);

void hashmap_static_delete_entries(void *static_hashmap);

bool hashmap_static_get(void *static_hashmap, struct hashmap_key *key, void **value);
bool hashmap_static_set(void *static_hashmap, struct hashmap_key *key, void *value);
bool hashmap_static_delete(void *static_hashmap, struct hashmap_key *key);

#endif

struct hashmap {
	__hashmap_header;

	unsigned char buf[];
};

#undef __hashmap_header

struct hashmap *hashmap_create(
	size_t n_entries,
	size_t n_divisions,
	hashmap_drop_handler drop_handler
);

struct hashmap *hashmap_copy_ref(struct hashmap *hashmap);
struct hashmap *hashmap_move_ref(struct hashmap **src);
void hashmap_destroy_ref(struct hashmap **src);

void hashmap_key_initialise(struct hashmap_key *key);
void hashmap_key_obtain(
	struct hashmap *hashmap,
	struct hashmap_key *hmap_key,
	const unsigned char *key,
	size_t key_sz
);
void hashmap_key_release(struct hashmap *hashmap, struct hashmap_key *key, bool hold_lock);

bool hashmap_get(struct hashmap *hashmap, struct hashmap_key *key, void **value);
bool hashmap_set(struct hashmap *hashmap, struct hashmap_key *key, void *value);
bool hashmap_delete(struct hashmap *hashmap, struct hashmap_key *key);
#endif
