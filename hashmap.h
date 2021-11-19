#ifndef HASHMAP_H
#define HASHMAP_H

#ifndef HASHMAP_N_ENTRIES
#define HASHMAP_N_ENTRIES 6151
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
};

unsigned int hashmap_hash(char *key, size_t key_length);
struct hashmap *hashmap_create(void (*entry_deletion_processor)(void *value));
void hashmap_destroy(struct hashmap *hashmap);
struct hashmap_entry **hashmap_internal_find(struct hashmap_entry **entry, char *key, size_t key_length);
unsigned char hashmap_internal_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast);
unsigned char hashmap_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast);
unsigned char hashmap_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast);
unsigned char hashmap_internal_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast);
void *hashmap_internal_get_wl(struct hashmap *hashmap, char *key, size_t key_length);
void *hashmap_get_wl(struct hashmap *hashmap, char *key, size_t key_length);
void *hashmap_get(struct hashmap *hashmap, char *key);
void *hashmap_internal_get(struct hashmap *hashmap, char *key);
void hashmap_internal_delete(struct hashmap_entry **entry, void (*processor)(void *));
void hashmap_delete_wl(struct hashmap *hashmap, char *key, size_t key_length);
void hashmap_delete(struct hashmap *hashmap, char *key);
void hashmap_lock(struct hashmap *hashmap);
void hashmap_unlock(struct hashmap *hashmap);

#endif