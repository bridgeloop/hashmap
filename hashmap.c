#include "hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

unsigned int hashmap_hash(char *key, size_t key_length) {
	unsigned int hash = 0;
	while (key_length--) {
		hash += *(key++);
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

struct hashmap *hashmap_create(void (*entry_deletion_processor)(void *value)) {
	struct hashmap *hashmap = malloc(sizeof(struct hashmap));
 	if (hashmap == NULL) {
 	 	return NULL;
 	}
	hashmap->entry_deletion_processor = entry_deletion_processor;
	if (pthread_mutex_init(&(hashmap->mutex), NULL) != 0) {
		free(hashmap);
		return NULL;
	}
	return hashmap;
}
void hashmap_destroy(struct hashmap *hashmap) {
	pthread_mutex_lock(&(hashmap->mutex));
	pthread_mutex_destroy(&(hashmap->mutex));
	for (unsigned short int entry = 0; entry < HASHMAP_N_ENTRIES; ++entry) {
		while (hashmap->entries[entry] != NULL) {
			hashmap_internal_delete(&(hashmap->entries[entry]), hashmap->entry_deletion_processor);
		}
	}
	free(hashmap);
}

void hashmap_lock(struct hashmap *hashmap) {
	pthread_mutex_lock(&(hashmap->mutex));
}

void hashmap_unlock(struct hashmap *hashmap) {
	pthread_mutex_unlock(&(hashmap->mutex));
}

struct hashmap_entry **hashmap_internal_find(struct hashmap_entry **entry, char *key, size_t key_length) {
	for (; *entry != NULL; entry = &((*entry)->next)) {
		if (key_length == (*entry)->key_length && strncmp(key, (*entry)->key, key_length) == 0) {
			return entry;
		}
	}
	return NULL;
}

unsigned char hashmap_internal_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast) {
	key = strndup(key, key_length);
	if (key == NULL) {
		return 0;
	}
	struct hashmap_entry *new_entry = malloc(sizeof(struct hashmap_entry));
	if (new_entry == NULL) {
		free(key);
		return 0;
	}
	struct hashmap_entry **entries = &(hashmap->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]);
	if (!fast) {
		struct hashmap_entry **entry = hashmap_internal_find(entries, key, key_length);
		if (entry != NULL) {
			hashmap_internal_delete(entry, hashmap->entry_deletion_processor);
		}
	}
	new_entry->key = key;
	new_entry->key_length = key_length;
	new_entry->value = value;
	new_entry->next = *entries;
	*entries = new_entry;
	return 1;
}
unsigned char hashmap_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast) {
	pthread_mutex_lock(&(hashmap->mutex));
	unsigned char r = hashmap_internal_set_wl(hashmap, key, key_length, value, fast);
	pthread_mutex_unlock(&(hashmap->mutex));
	return r;
}
unsigned char hashmap_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast) {
	return hashmap_set_wl(hashmap, key, strlen(key), value, fast);
}
unsigned char hashmap_internal_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast) {
	return hashmap_internal_set_wl(hashmap, key, strlen(key), value, fast);
}

void *hashmap_internal_get_wl(struct hashmap *hashmap, char *key, size_t key_length) {
	struct hashmap_entry **value = hashmap_internal_find(&(hashmap->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]), key, key_length);
	if (value == NULL) {
		return NULL;
	}
	return (*value)->value;
}
void *hashmap_get_wl(struct hashmap *hashmap, char *key, size_t key_length) {
	pthread_mutex_lock(&(hashmap->mutex));
	void *value = hashmap_internal_get_wl(hashmap, key, key_length);
	pthread_mutex_unlock(&(hashmap->mutex));
	return value;
}
void *hashmap_get(struct hashmap *hashmap, char *key) {
	return hashmap_get_wl(hashmap, key, strlen(key));
}
void *hashmap_internal_get(struct hashmap *hashmap, char *key) {
	return hashmap_internal_get_wl(hashmap, key, strlen(key));
}

void hashmap_internal_delete(struct hashmap_entry **entry, void (*processor)(void *)) {
	struct hashmap_entry *d_entry = *entry;
	free(d_entry->key);
	if (processor != NULL) {
		processor(d_entry->value);
	}
	*entry = d_entry->next;
	free(d_entry);
}
void hashmap_delete_wl(struct hashmap *hashmap, char *key, size_t key_length) {
	pthread_mutex_lock(&(hashmap->mutex));
	struct hashmap_entry **entry = hashmap_internal_find(&(hashmap->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]), key, key_length);
	if (entry != NULL) {
		hashmap_internal_delete(entry, hashmap->entry_deletion_processor);
	}
	pthread_mutex_unlock(&(hashmap->mutex));
}
void hashmap_delete(struct hashmap *hashmap, char *key) {
	hashmap_delete_wl(hashmap, key, strlen(key));
}

#ifdef HASHMAP_MAIN
#include <stdio.h>
int main(void) {
	struct hashmap *hashmap = hashmap_create(NULL);
	hashmap_set(hashmap, "test", (void *)0xabcd, 0);
	printf("%p\n", hashmap_get(hashmap, "test"));
	hashmap_destroy(hashmap);
	return 0;
}
#endif
