#ifndef HASHMAP_H
#define HASHMAP_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

struct hashmap_bucket {
	struct hashmap_entry *entries;
	struct hashmap_bucket **prev_next;
	struct hashmap_bucket *next;
};

struct hashmap_key {
	const unsigned char *key;
	size_t key_sz;
	struct hashmap_bucket *bucket;
};
#define HASHMAP_KEY_INITIALIZER { .bucket = NULL }

struct hashmap_entry {
	void *value;
	struct hashmap_entry *next;

	size_t key_sz;
	unsigned char key[];
};

enum hashmap_drop_mode {
	hashmap_drop_set,
	hashmap_drop_delete,
};

struct hashmap {
	void (*drop_handler)(void *value, enum hashmap_drop_mode drop_mode);
	size_t n_divisions;
	size_t n_buckets;

	pthread_mutex_t meta_mutex;
	struct hashmap_bucket *bucket_with_entries;
	size_t ref_count;

	unsigned char buf[];
};

struct hashmap *hashmap_create(
	size_t n_entries,
	size_t n_divisions,
	void (*drop_handler)(void *value, enum hashmap_drop_mode drop_mode)
);

struct hashmap *hashmap_copy_ref(struct hashmap *hashmap);
struct hashmap *hashmap_move_ref(struct hashmap **src);
void hashmap_destroy_ref(struct hashmap **src);

void hashmap_key_initialise(struct hashmap_key *key);
void hashmap_key_release(struct hashmap *hashmap, struct hashmap_key *key, bool hold_lock);
void hashmap_key_obtain(
	struct hashmap *hashmap,
	struct hashmap_key *hmap_key,
	const unsigned char *key,
	size_t key_sz
);

bool hashmap_get(struct hashmap *hashmap, struct hashmap_key *key, void **value);
bool hashmap_set(struct hashmap *hashmap, struct hashmap_key *key, void *value);
bool hashmap_delete(struct hashmap *hashmap, struct hashmap_key *key);

#endif
