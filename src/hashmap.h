/*
	ISC License
	
	Copyright (c) 2023, aiden (aiden@cmp.bz)
	
	Permission to use, copy, modify, and/or distribute this software for any
	purpose with or without fee is hereby granted, provided that the above
	copyright notice and this permission notice appear in all copies.
	
	THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
	WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
	MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
	ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
	WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
	ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
	OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef HASHMAP_H
#define HASHMAP_H

// this hashmap _can_ be very fast with
// the right hash function. the only real
// bottleneck here is malloc, which is used on
// insert because you cannot enforce lifetimes in c.
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

#if __has_builtin(__builtin_ia32_pause)
#define hashmap_mpause() __builtin_ia32_pause()
#elif __has_builtin(__builtin_arm_isb)
#define hashmap_mpause() __builtin_arm_isb(15)
#elif __has_builtin(__builtin_arm_yield)
#define hashmap_mpause() __builtin_arm_yield()
#else
#define hashmap_mpause() (void)0
#endif

#include "ifc/ifc.h"

#define HASHMAP_MIN_RESERVE 24
enum hashmap_callback_reason {
	hashmap_acquire,

	hashmap_drop_destroy,
	hashmap_drop_delete,
	hashmap_drop_set,
};
typedef void (*hashmap_callback)(void *entry, enum hashmap_callback_reason reason, void *arg);

struct hashmap_key {
	void *key;
	uint32_t key_sz;

	uint32_t hash;
};

struct hashmap_kv {
	void *value;

	uint32_t key_sz;
	unsigned char key[];
};

struct hashmap_bucket_protected {
	/*
		"probe sequence length";
		how many failed entry comparisons it takes to find
		this entry in the buckets array via linear probing


		i.e., assuming that the entry exists in the hashmap:
		the distance (to the right, wrapping) from the bucket at index
		(hash % n_buckets), to the desired bucket

		e.g. (still assuming that the entry exists in the hashmap):
		the return value of the following pseudo-code function:
		uint32_t psl() {
			for (uint32_t it = 0;; ++it) {
				if (desired_entry == &(buckets[((hash % n_buckets) + it) % n_buckets])) {
					return it;
				}
			}
			unreachable!
		}
	*/

	// to-do: psl u16, compute if doesn't fit
	uint32_t psl;
	uint32_t hash;
	struct hashmap_kv *kv;
};

struct hashmap_bucket {
	atomic_flag lock;
	struct hashmap_bucket_protected protected;
};

struct hashmap_area {
	uint32_t reserved;
	atomic_bool lock;
};

struct hashmap {
	_Atomic size_t reference_count;
	const hashmap_callback callback;

	struct hashmap_bucket *buckets;
	uint32_t n_buckets;
	_Atomic uint32_t occupied_buckets;

	const float
		resize_percentage,
		resize_multiply;
		
	// resize //

	atomic_bool resize_fail;
	atomic_bool resizing;
	uint16_t threads_resizing;

	_Atomic uint32_t init_idx;
	_Atomic uint32_t resize_idx;

	pthread_mutex_t resize_mutex;
	pthread_cond_t resize_cond;
	pthread_mutex_t ensured_mutex;
	pthread_cond_t ensured_cond;
	pthread_cond_t stop_resize_cond;

	struct hashmap_bucket *new_buckets;
	uint32_t new_n_buckets;

	// ifc //

	struct ifc *const ifc;
};

// *output_bucket will **always** be set to a locked hashmap bucket.
// it is the caller's duty to release the bucket's lock once it is done using *output_bucket.
static bool _hashmap_find(
	struct hashmap_bucket *buckets,
	uint32_t n_buckets,

	struct hashmap_key *hm_key,

	struct hashmap_bucket **output_bucket,
	uint32_t *psl
) {
	*psl = 0;

	void *key = hm_key->key;
	uint32_t key_sz = hm_key->key_sz;
	uint32_t hash = hm_key->hash;
	uint32_t bucket_idx = hm_key->hash % n_buckets;

	struct hashmap_bucket *sentinel = &(buckets[n_buckets]);

	struct hashmap_bucket *bucket = &(buckets[bucket_idx]);
	while (__atomic_test_and_set(&(bucket->lock), __ATOMIC_ACQUIRE)) {
		hashmap_mpause();
	}

	for (size_t it = 0;; ++it) {
		struct hashmap_bucket_protected *protected = &(bucket->protected);
		if (
			protected->kv == NULL ||
			protected->psl < *psl
		) {
			*output_bucket = bucket;
			return false;
		}
		if (protected->hash == hash && protected->kv->key_sz == key_sz) {
			if (memcmp(key, protected->kv->key, key_sz) == 0) {
				// found entry
				*output_bucket = bucket;
				return true;
			}
		}

		*psl += 1;
		struct hashmap_bucket *next_bucket = bucket + 1;
		if (next_bucket == sentinel) {
			next_bucket = buckets;
		}

		while (__atomic_test_and_set(&(next_bucket->lock), __ATOMIC_ACQUIRE)) {
			hashmap_mpause();
		}
		__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE);

		bucket = next_bucket;
	}
}

static void _hashmap_cfi(
	struct hashmap_bucket *array,
	struct hashmap_bucket **current,
	struct hashmap_bucket *sentinel,

	struct hashmap_bucket_protected interior
) {
	struct hashmap_bucket_protected swap_prot;

	swap_prot = (*current)->protected;
	(*current)->protected = interior;
	interior = swap_prot;

	if (interior.kv == NULL) {
		return;
	}

	for (;;) {
		atomic_flag *old_lock = &((*current)->lock);
		(*current) += 1;
		if ((*current) == sentinel) {
			(*current) = array;
		}
		while (__atomic_test_and_set(&((*current)->lock), __ATOMIC_ACQUIRE)) {
			hashmap_mpause();
		}
		__atomic_clear(old_lock, __ATOMIC_RELEASE);

		interior.psl += 1;

		if ((*current)->protected.kv == NULL) {
			(*current)->protected = interior;
			return;
		}

		if ((*current)->protected.psl < interior.psl) {
			swap_prot = (*current)->protected;
			(*current)->protected = interior;
			interior = swap_prot;
		}
	}
}

static void _hashmap_resize(struct hashmap *hashmap, struct hashmap_area *area, bool is_main_thread) {
	if (hashmap->resize_fail) {
		return;
	}

	area->lock = false;

	struct hashmap_bucket *buckets, *new_buckets;
	size_t n_buckets, new_n_buckets;	
	if (is_main_thread) {
		buckets = hashmap->buckets;
		n_buckets = hashmap->n_buckets;

		new_n_buckets = n_buckets * hashmap->resize_multiply;
		// allocate new buckets array
		if (
			(new_buckets = malloc(new_n_buckets * sizeof(struct hashmap_bucket))) == NULL
		) {
			area->lock = true;
			hashmap->resize_fail = true;
			__atomic_clear(&(hashmap->resizing), __ATOMIC_RELEASE);
			pthread_mutex_lock(&(hashmap->ensured_mutex));
			pthread_cond_broadcast(&(hashmap->ensured_cond));
			pthread_mutex_unlock(&(hashmap->ensured_mutex));
			return;
		}
		hashmap->new_buckets = new_buckets;
		hashmap->new_n_buckets = new_n_buckets;
		hashmap->init_idx = 0;
		hashmap->resize_idx = 0;

		// to-do: maybe make this concurrent
		uint32_t n = new_n_buckets / *(unsigned int *)hashmap->ifc;
		for (;;) {
			uint32_t idx = (hashmap->init_idx += n) - n;
			if (idx >= new_n_buckets) {
				break;
			}
			if (idx + n >= new_n_buckets) {
				n = new_n_buckets - idx;
			}

			for (uint32_t it = 0; it < n; ++it) {
				struct hashmap_bucket *bucket = &(new_buckets[idx + it]);
				__atomic_clear(&(bucket->lock), __ATOMIC_RELAXED);
				bucket->protected.kv = NULL;
			}
		}

		// wait for all other threads to
		// leave non-resize critical sections
		pthread_mutex_lock(&(hashmap->resize_mutex));
		wait:;
		// wait for other threads to stop working
		ifc_iter(struct hashmap_area)(hashmap->ifc, it_area) {
			if (it_area->lock) {
				pthread_cond_wait(&(hashmap->resize_cond), &(hashmap->resize_mutex));
				goto wait;
			}
		}
		pthread_mutex_unlock(&(hashmap->resize_mutex));
		pthread_mutex_lock(&(hashmap->ensured_mutex));
		assert(hashmap->threads_resizing == 0);
		hashmap->threads_resizing = 1;
		pthread_cond_broadcast(&(hashmap->ensured_cond));
		pthread_mutex_unlock(&(hashmap->ensured_mutex));
	} else {
		pthread_mutex_lock(&(hashmap->resize_mutex));
		pthread_cond_signal(&(hashmap->resize_cond));
		pthread_mutex_unlock(&(hashmap->resize_mutex));
		pthread_mutex_lock(&(hashmap->ensured_mutex));
		while (hashmap->threads_resizing == 0) {
			if (!hashmap->resizing) {
				area->lock = true;
				pthread_mutex_unlock(&(hashmap->ensured_mutex));
				return;
			}
			pthread_cond_wait(&(hashmap->ensured_cond), &(hashmap->ensured_mutex));
		}
		hashmap->threads_resizing += 1;
		pthread_mutex_unlock(&(hashmap->ensured_mutex));

		buckets = hashmap->buckets;
		n_buckets = hashmap->n_buckets;
		new_buckets = hashmap->new_buckets;
		new_n_buckets = hashmap->new_n_buckets;
	}

	// hashmap->threads_resizing != 0, so this is safe
	area->lock = true;

	// assist with the resize
	uint32_t n = n_buckets / *(unsigned int *)hashmap->ifc;
	for (;;) {
		// to-do: integer overflow
		uint32_t idx = (hashmap->resize_idx += n) - n;
		if (idx >= n_buckets) {
			break;
		}
		if (idx + n >= n_buckets) {
			n = n_buckets - idx;
		}

		for (uint32_t it = 0; it < n; ++it) {
			struct hashmap_bucket_protected *prot =
				&(buckets[idx + it].protected);
			if (prot->kv == NULL) {
				continue;
			}

			struct hashmap_bucket *bucket;
			uint32_t psl;

			struct hashmap_key key = {
				.key = prot->kv->key,
				.key_sz = prot->kv->key_sz,

				.hash = prot->hash,
			};

			_hashmap_find(
				new_buckets,
				new_n_buckets,

				&(key),

				&(bucket),
				&(psl)
			);
			_hashmap_cfi(
				new_buckets, &(bucket), &(new_buckets[new_n_buckets]),
				(struct hashmap_bucket_protected){
					.kv = prot->kv,
					.hash = prot->hash,

					.psl = psl,
				}
			);

			__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE);
		}
	}

	pthread_mutex_lock(&(hashmap->ensured_mutex));
	if (--hashmap->threads_resizing == 0) {
		free(buckets);
		hashmap->buckets = new_buckets;
		hashmap->n_buckets = new_n_buckets;
		pthread_cond_broadcast(&(hashmap->stop_resize_cond));
		__atomic_clear(&(hashmap->resizing), __ATOMIC_RELEASE);
	} else {
		pthread_cond_wait(&(hashmap->stop_resize_cond), &(hashmap->ensured_mutex));
	}
	pthread_mutex_unlock(&(hashmap->ensured_mutex));

	return;
}

static size_t _hashmap_reserve(struct hashmap *hashmap, struct hashmap_area *area, uint32_t n_reserve, bool *resize_needed) {
	if (n_reserve == 0) {
		*resize_needed = false;
		return 0;
	}

	uint32_t n_buckets = hashmap->n_buckets;
	uint32_t capture = hashmap->occupied_buckets;
	uint32_t update;
	do {
		if (capture + n_reserve > n_buckets * hashmap->resize_percentage && !hashmap->resize_fail) {
			*resize_needed = true;
			return 0;
		}
		if (n_reserve > hashmap->n_buckets - capture) {
			update = hashmap->n_buckets - capture;
		} else {
			update = capture + n_reserve;
		}
	} while (!atomic_compare_exchange_weak_explicit(
		&(hashmap->occupied_buckets),
		&(capture),
		update,

		memory_order_relaxed,
		memory_order_relaxed
	));

	size_t reserved = update - capture;
	area->reserved += reserved;
	*resize_needed = false;
	return reserved;
}

static inline void _hashmap_not_running(struct hashmap *hashmap, struct hashmap_area *area) {
	area->lock = false;
	if (hashmap->resizing) {
		// to-do: maybe assist?
		pthread_mutex_lock(&(hashmap->resize_mutex));
		pthread_cond_signal(&(hashmap->resize_cond));
		pthread_mutex_unlock(&(hashmap->resize_mutex));
	}
	return;
}

static void hashmap_key(
	void *key,
	uint32_t key_sz,

	struct hashmap_key *output_key
) {
	if (key == NULL && key_sz != 0) {
		abort();
	}

	output_key->key = key;
	output_key->key_sz = key_sz;

	output_key->hash = HASHMAP_HASH_FUNCTION(key, key_sz);

	return;
}

static struct hashmap_area *hashmap_area(struct hashmap *hashmap) {
	return ifc_area(hashmap->ifc);
}
static void hashmap_area_flush(struct hashmap *hashmap, struct hashmap_area *area) {
	// does not require a lock
	hashmap->occupied_buckets -= area->reserved;
	area->reserved = 0;
	return;
}
static void hashmap_area_release(struct hashmap *hashmap, struct hashmap_area *area) {
	hashmap_area_flush(hashmap, area);
	ifc_release(hashmap->ifc, area);
	return;
}

static size_t hashmap_reserve(struct hashmap *hashmap, struct hashmap_area *area, size_t n_reserve) {
	assert(hashmap != NULL && area != NULL);

	// try to enter critical section
	// (conceptually a trylock)
	area->lock = true;
	if (hashmap->resizing) {
		// "trylock" failed, so we must
		// assist with the ongoing resize
		_hashmap_resize(hashmap, area, false);
		// area->lock is still true, and the
		// resize has completed, so we can enter
		// the critical section
	}

	reserve:;
	bool resize_needed;
	size_t reserved = _hashmap_reserve(hashmap, area, n_reserve, &(resize_needed));

	if (resize_needed) {
		bool acq = __atomic_test_and_set(&(hashmap->resizing), __ATOMIC_ACQUIRE) == false;
		_hashmap_resize(hashmap, area, acq);
		goto reserve;
	}

	_hashmap_not_running(hashmap, area);

	return reserved;
}

enum hashmap_cas_result {
	hashmap_cas_success,
	hashmap_cas_again,
	hashmap_cas_error,
};
enum hashmap_cas_option {
	hashmap_cas_set,
	hashmap_cas_delete,
	hashmap_cas_get,
};

static enum hashmap_cas_result hashmap_cas(
	struct hashmap *hashmap,
	struct hashmap_area *area,
	struct hashmap_key *key,

	void **expected_value,
	void *new_value,

	enum hashmap_cas_option option,
	void *callback_arg
) {
	assert(hashmap != NULL && area != NULL && key != NULL && expected_value != NULL);

	// try to enter critical section
	// (conceptually a trylock)
	area->lock = true;
	if (hashmap->resizing) {
		// "trylock" failed, so we must
		// assist with the ongoing resize
		_hashmap_resize(hashmap, area, false);
		// area->lock is still true, and the
		// resize has completed, so we can enter
		// the critical section
	}

	// enter critical section
	// this function cannot be interrupted
	// while in the critical section

	cas:;
	#define _hashmap_cas_leave_critical_section() do { \
		__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE); \
		_hashmap_not_running(hashmap, area); \
	} while (0);

	struct hashmap_bucket
		*buckets = hashmap->buckets,
		*bucket;
	uint32_t n_buckets = hashmap->n_buckets;

	uint32_t psl;

	bool find = _hashmap_find(
		buckets,
		n_buckets,

		key,

		&(bucket),
		&(psl)
	);

	if (find) {
		void **current_value = &(bucket->protected.kv->value);
		if (option == hashmap_cas_delete) {
			if (new_value == NULL && *expected_value != *current_value) {
				*expected_value = *current_value;
				_hashmap_cas_leave_critical_section();
				return hashmap_cas_again;
			}
			if (hashmap->callback != NULL) {
				hashmap->callback(*current_value, hashmap_drop_delete, callback_arg);
			}
			free(bucket->protected.kv);
			bucket->protected.kv = NULL;

			struct hashmap_bucket *sentinel = &(buckets[n_buckets]);
			for (;;) {
				struct hashmap_bucket *next_bucket = bucket + 1;
				if (next_bucket == sentinel) {
					next_bucket = buckets;
				}
				while (__atomic_test_and_set(&(next_bucket->lock), __ATOMIC_ACQUIRE)) {
					hashmap_mpause();
				}
				if (next_bucket->protected.kv == NULL || next_bucket->protected.psl == 0) {
					__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE);
					__atomic_clear(&(next_bucket->lock), __ATOMIC_RELEASE);
					break;
				}
				bucket->protected = next_bucket->protected;
				__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE);
				bucket = next_bucket;
			}

			area->reserved += 1;

			_hashmap_cas_leave_critical_section();
			return hashmap_cas_success;
		}
		if (
			(option == hashmap_cas_set && *expected_value != *current_value) ||
			option == hashmap_cas_get
		) {
			if (hashmap->callback != NULL) {
				hashmap->callback(*current_value, hashmap_acquire, callback_arg);
			}
			*expected_value = *current_value;
			_hashmap_cas_leave_critical_section();
			return hashmap_cas_again;
		}
		if (hashmap->callback != NULL) {
			hashmap->callback(*current_value, hashmap_drop_set, callback_arg);
		}
		*current_value = new_value;
		_hashmap_cas_leave_critical_section();
		return hashmap_cas_success;
	}

	if (option != hashmap_cas_set) {
		_hashmap_cas_leave_critical_section();
		return hashmap_cas_error;
	}

	if (area->reserved == 0) {
		bool resize_needed;
		if (_hashmap_reserve(hashmap, area, HASHMAP_MIN_RESERVE, &(resize_needed)) == 0) {
			if (resize_needed) {
				__atomic_clear(&(bucket->lock), __ATOMIC_RELEASE);
				bool acq = __atomic_test_and_set(&(hashmap->resizing), __ATOMIC_ACQUIRE) == false;
				_hashmap_resize(hashmap, area, acq);
				// even if the resize failed, the bucket
				// may have been inserted by another thread
				// after we released our exclusive control
				// over the bucket (key) via atomic_clear.
				goto cas;
			}
			_hashmap_cas_leave_critical_section();
			return hashmap_cas_error;
		}
	}

	// allocate kv (probably a bottleneck)
	struct hashmap_kv *kv = malloc(
		sizeof(struct hashmap_kv) +
		key->key_sz
	);
	if (kv == NULL) {
		_hashmap_cas_leave_critical_section();
		return hashmap_cas_error;
	}
	area->reserved -= 1;
	kv->value = new_value;
	kv->key_sz = key->key_sz;
	memcpy(kv->key, key->key, key->key_sz);

	_hashmap_cfi(
		buckets, &(bucket), &(buckets[n_buckets]),
		(struct hashmap_bucket_protected){
			.hash = key->hash,
			.psl = psl,

			.kv = kv,
		}
	);

	_hashmap_cas_leave_critical_section();
	return hashmap_cas_success;
}

static struct hashmap *hashmap_create(
	uint16_t n_threads,
	uint32_t initial_size,
	float resize_percentage,
	float resize_multiply,

	hashmap_callback callback
) {
	if (n_threads == 0) {
		return NULL;
	}
	if (resize_percentage <= 0 || resize_percentage > 1) {
		resize_percentage = 0.94;
	}
	if (resize_multiply <= 1) {
		resize_multiply = 2;
	}

	size_t n_buckets = n_threads + 1;

	float _min = HASHMAP_MIN_RESERVE / resize_percentage;
	uint32_t min = ((uint32_t)_min + (_min == (uint32_t)_min ? 0 : 1));

	if (initial_size < min) {
		initial_size = min;
	}
	if (n_buckets < initial_size) {
		n_buckets = initial_size;
	}

	struct hashmap *hashmap = malloc(sizeof(struct hashmap));
	if (hashmap == NULL) {
		return NULL;
	}
	struct hashmap_bucket *buckets = malloc(sizeof(struct hashmap_bucket) * n_buckets);
	if (buckets == NULL) {
		err1:;
		free(hashmap);
		return NULL;
	}
	*(struct ifc **)&(hashmap->ifc) = ifc_alloc(n_threads, sizeof(size_t));
	if (hashmap->ifc == NULL) {
		err2:;
		free(buckets);
		goto err1;
	}
	if (pthread_mutex_init(&(hashmap->resize_mutex), NULL) != 0) {
		err3:;
		ifc_free(hashmap->ifc);
		goto err2;
	}
	if (pthread_cond_init(&(hashmap->resize_cond), NULL) != 0) {
		err4:;
		pthread_mutex_destroy(&(hashmap->resize_mutex));
		goto err3;
	}
	if (pthread_mutex_init(&(hashmap->ensured_mutex), NULL) != 0) {
		err5:;
		pthread_cond_destroy(&(hashmap->resize_cond));
		goto err4;
	}
	if (pthread_cond_init(&(hashmap->ensured_cond), NULL) != 0) {
		err6:;
		pthread_mutex_destroy(&(hashmap->ensured_mutex));
		goto err5;
	}
	if (pthread_cond_init(&(hashmap->stop_resize_cond), NULL) != 0) {
		pthread_cond_destroy(&(hashmap->resize_cond));
		goto err6;
	}

	hashmap->reference_count = 1;

	*(hashmap_callback *)&(hashmap->callback) = callback;

	hashmap->buckets = buckets;
	hashmap->n_buckets = n_buckets;
	hashmap->occupied_buckets = 0;
	for (size_t idx = 0; idx < n_buckets; ++idx) {
		buckets[idx].protected.kv = NULL;
		__atomic_clear(&(buckets[idx].lock), __ATOMIC_RELAXED);
	}

	*(float *)&(hashmap->resize_percentage) = resize_percentage;
	*(float *)&(hashmap->resize_multiply) = resize_multiply;

	// resize
	__atomic_clear(&(hashmap->resize_fail), __ATOMIC_RELAXED);
	__atomic_clear(&(hashmap->resizing), __ATOMIC_RELAXED);
	hashmap->threads_resizing = 0;

	// ifc
	ifc_iter(struct hashmap_area)(hashmap->ifc, area) {
		area->reserved = 0;
		area->lock = false;
	}

	return hashmap;
}

static struct hashmap *hashmap_copy_ref(struct hashmap *hashmap) {
	hashmap->reference_count += 1;
	return hashmap;
}

static void hashmap_destroy(struct hashmap *hashmap) {
	if (--hashmap->reference_count == 0) {
		pthread_cond_destroy(&(hashmap->stop_resize_cond));
		pthread_cond_destroy(&(hashmap->resize_cond));
		pthread_mutex_destroy(&(hashmap->ensured_mutex));
		pthread_cond_destroy(&(hashmap->resize_cond));
		pthread_mutex_destroy(&(hashmap->resize_mutex));

		ifc_free(hashmap->ifc);

		for (size_t idx = 0; hashmap->occupied_buckets != 0; ++idx) {
			struct hashmap_bucket_protected *prot = &(hashmap->buckets[idx].protected);
			if (prot->kv != NULL) {
				if (hashmap->callback != NULL) {
					hashmap->callback(prot->kv->value, hashmap_drop_destroy, NULL);
				}
				free(prot->kv);
				hashmap->occupied_buckets -= 1;
			}
		}

		free(hashmap->buckets);
		free(hashmap);
	}
	return;
}

#endif
