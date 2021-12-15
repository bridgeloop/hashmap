#include "hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define HASHMAP_LH_C(x) ((struct hashmap *)x)

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
	hashmap->ref_count = 1;
	if (pthread_mutex_init(&(hashmap->mutex), NULL) != 0) {
		free(hashmap);
		return NULL;
	}
	return hashmap;
}

void locked_hashmap_destroy(struct locked_hashmap **locked_hashmap) {
	struct hashmap *hashmap = HASHMAP_LH_C(*locked_hashmap);
	pthread_mutex_destroy(&(hashmap->mutex));
	for (unsigned short int entry = 0; entry < HASHMAP_N_ENTRIES; ++entry) {
		while (hashmap->entries[entry] != NULL) {
			locked_hashmap_remove(&(hashmap->entries[entry]), hashmap->entry_deletion_processor);
		}
	}
	free(hashmap);
}
void hashmap_destroy(struct hashmap *hashmap) {
	struct locked_hashmap *locked_hashmap = hashmap_lock(hashmap);
	locked_hashmap_destroy(&(locked_hashmap));
}

unsigned char locked_hashmap_ref_plus(struct locked_hashmap **locked_hashmap, int b) {
	unsigned int *rc = &(HASHMAP_LH_C(*locked_hashmap)->ref_count);
	unsigned long long int chk = (unsigned long long int)(*rc) + (unsigned long long int)(b);
	if (chk > (unsigned int)(~0)) {
		return 0;
	}
	if ((*rc = chk) == 0) {
		locked_hashmap_destroy(locked_hashmap);
		return 2;
	}
	return 1;
}
unsigned char hashmap_ref_plus(struct hashmap *hashmap, int b) {
	struct locked_hashmap *locked_hashmap = hashmap_lock(hashmap);
	unsigned char r = locked_hashmap_ref_plus(&(locked_hashmap), b);
	if (r != 2) {
		locked_hashmap_unlock(&(locked_hashmap));
	}
	return r;
}


struct locked_hashmap *hashmap_lock(struct hashmap *hashmap) {
	pthread_mutex_lock(&(hashmap->mutex));
	return (struct locked_hashmap *)hashmap;
}
void locked_hashmap_unlock(struct locked_hashmap **locked_hashmap) {
	pthread_mutex_unlock(&((HASHMAP_LH_C(*locked_hashmap))->mutex));
	*locked_hashmap = NULL;
}

struct hashmap_entry **locked_hashmap_find(struct hashmap_entry **entry, char *key, size_t key_length) {
	for (; *entry != NULL; entry = &((*entry)->next)) {
		if (key_length == (*entry)->key_length && strncmp(key, (*entry)->key, key_length) == 0) {
			return entry;
		}
	}
	return NULL;
}

unsigned char locked_hashmap_set_wl(struct locked_hashmap *locked_hashmap, char *key, size_t key_length, void *value, unsigned char fast) {
	key = strndup(key, key_length);
	if (key == NULL) {
		return 0;
	}
	struct hashmap_entry *new_entry = malloc(sizeof(struct hashmap_entry));
	if (new_entry == NULL) {
		free(key);
		return 0;
	}
	struct hashmap_entry **entries = &(HASHMAP_LH_C(locked_hashmap)->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]);
	if (!fast) {
		struct hashmap_entry **entry = locked_hashmap_find(entries, key, key_length);
		if (entry != NULL) {
			locked_hashmap_remove(entry, HASHMAP_LH_C(locked_hashmap)->entry_deletion_processor);
		}
	}
	new_entry->key = key;
	new_entry->key_length = key_length;
	new_entry->value = value;
	new_entry->next = *entries;
	*entries = new_entry;
	return 1;
}
unsigned char locked_hashmap_set(struct locked_hashmap *hashmap, char *key, void *value, unsigned char fast) {
	return locked_hashmap_set_wl(hashmap, key, strlen(key), value, fast);
}
unsigned char hashmap_set_wl(struct hashmap *hashmap, char *key, size_t key_length, void *value, unsigned char fast) {
	struct locked_hashmap *locked_hashmap = hashmap_lock(hashmap);
	unsigned char r = locked_hashmap_set_wl(locked_hashmap, key, key_length, value, fast);
	locked_hashmap_unlock(&(locked_hashmap));
	return r;
}
unsigned char hashmap_set(struct hashmap *hashmap, char *key, void *value, unsigned char fast) {
	return hashmap_set_wl(hashmap, key, strlen(key), value, fast);
}

void *locked_hashmap_get_wl(struct locked_hashmap *hashmap, char *key, size_t key_length) {
	struct hashmap_entry **value = locked_hashmap_find(&(HASHMAP_LH_C(hashmap)->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]), key, key_length);
	if (value == NULL) {
		return NULL;
	}
	return (*value)->value;
}
void *locked_hashmap_get(struct locked_hashmap *hashmap, char *key) {
	return locked_hashmap_get_wl(hashmap, key, strlen(key));
}
void *hashmap_get_wl(struct hashmap *hashmap, char *key, size_t key_length) {
	struct locked_hashmap *locked_hashmap = hashmap_lock(hashmap);
	void *value = locked_hashmap_get_wl(locked_hashmap, key, key_length);
	locked_hashmap_unlock(&(locked_hashmap));
	return value;
}
void *hashmap_get(struct hashmap *hashmap, char *key) {
	return hashmap_get_wl(hashmap, key, strlen(key));
}

void locked_hashmap_remove(struct hashmap_entry **entry, void (*processor)(void *)) {
	struct hashmap_entry *d_entry = *entry;
	free(d_entry->key);
	if (processor != NULL) {
		processor(d_entry->value);
	}
	*entry = d_entry->next;
	free(d_entry);
}
void locked_hashmap_delete_wl(struct locked_hashmap *locked_hashmap, char *key, size_t key_length) {
	struct hashmap_entry **entry = locked_hashmap_find(&(HASHMAP_LH_C(locked_hashmap)->entries[hashmap_hash(key, key_length) % HASHMAP_N_ENTRIES]), key, key_length);
	if (entry != NULL) {
		locked_hashmap_remove(entry, HASHMAP_LH_C(locked_hashmap)->entry_deletion_processor);
	}
}
void locked_hashmap_delete(struct locked_hashmap *locked_hashmap, char *key) {
	locked_hashmap_delete_wl(locked_hashmap, key, strlen(key));
}
void hashmap_delete_wl(struct hashmap *hashmap, char *key, size_t key_length) {
	struct locked_hashmap *locked_hashmap = hashmap_lock(hashmap);
	locked_hashmap_delete_wl(locked_hashmap, key, key_length);
	locked_hashmap_unlock(&(locked_hashmap));
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
