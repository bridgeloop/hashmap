#ifndef HASHMAP_H
#define HASHMAP_H

#ifndef HASHMAP_N_ENTRIES
#define HASHMAP_N_ENTRIES 193
#endif

#include <pthread.h>

struct hashmap_entry {
	char *key;
	size_t key_length;
	void *value;
	struct hashmap_entry *next;
};

struct hashmap {
	pthread_mutex_t mutex;
	struct hashmap_entry *entries[HASHMAP_N_ENTRIES];
	void (*entry_deletion_processor)(void *value);
	unsigned int ref_count;
};
struct locked_hashmap; // for the type itself

unsigned int hashmap_hash(char *key, size_t key_length);

struct hashmap *hashmap_create(void (*entry_deletion_processor)(void *value));

void locked_hashmap_destroy(struct locked_hashmap **locked_hashmap);
void hashmap_destroy(struct hashmap *hashmap);

unsigned char locked_hashmap_ref_plus(struct locked_hashmap **locked_hashmap, int b);
unsigned char hashmap_ref_plus(struct hashmap *hashmap, int b);

struct hashmap_entry **locked_hashmap_find(struct hashmap_entry **entry, char *key, size_t key_length);
void locked_hashmap_remove(struct hashmap_entry **entry, void (*processor)(void *));

unsigned char locked_hashmap_set_wl(struct locked_hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast);
unsigned char hashmap_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast);
unsigned char hashmap_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast);
unsigned char locked_hashmap_set(struct locked_hashmap *hashmap, char *key, void *value, unsigned char fast);

void *locked_hashmap_get_wl(struct locked_hashmap *hashmap, char *key, size_t key_length);
void *hashmap_get_wl(struct hashmap *hashmap, char *key, size_t key_length);
void *locked_hashmap_get(struct locked_hashmap *hashmap, char *key);
void *hashmap_get(struct hashmap *hashmap, char *key);

void locked_hashmap_delete_wl(struct locked_hashmap *locked_hashmap, char *key, size_t key_length);
void locked_hashmap_delete(struct locked_hashmap *locked_hashmap, char *key);
void hashmap_delete_wl(struct hashmap *hashmap, char *key, size_t key_length);
void hashmap_delete(struct hashmap *hashmap, char *key);

struct locked_hashmap *hashmap_lock(struct hashmap *hashmap);
void locked_hashmap_unlock(struct locked_hashmap **locked_hashmap);

#endif
