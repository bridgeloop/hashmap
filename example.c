#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include <time.h>
double rc(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &(now));
    return now.tv_sec + (now.tv_nsec * 1e-9);
}

#define HASHMAP_HASH_FUNCTION(key, key_sz) (*(uint64_t *)key ^ 9268326398)
#include "src/hashmap.h"

#define N_THREADS 8
#define N_BUCKETS 24000000

struct hashmap *the_hashmap;

void *writet(void *_) {
	while (nolock) hashmap_mpause();

	static atomic_uint_fast32_t chunk = 0;
	static const uint_fast32_t CHUNK_SZ = 1024;

	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);

	for (;;) {
		uint_fast32_t
			end = (chunk += CHUNK_SZ),
			start = end - CHUNK_SZ;
		if (end > N_BUCKETS) {
			end -= end - N_BUCKETS;
		}
		if (start >= N_BUCKETS) {
			hashmap_area_release(the_hashmap, area);
			return NULL;
		}

		for (uint_fast32_t idx = start; idx < end; ++idx) {
			hashmap_key(&(idx), sizeof(idx), &(key));
			if (hashmap_cas(
				the_hashmap, area, &(key),
				&(x), (void *)idx,
				hashmap_cas_set, NULL
			) == hashmap_cas_error) {
				puts("error!");
				exit(1);
			}
		}
	}
}

void *readt(void *_) {
	//while (!nolock) hashmap_mpause();

	static atomic_uint_fast32_t chunk = 0;
	static const uint_fast32_t CHUNK_SZ = 1024;

	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);

	for (;;) {
		uint_fast32_t
			end = (chunk += CHUNK_SZ),
			start = end - CHUNK_SZ;
		if (end > N_BUCKETS) {
			end -= end - N_BUCKETS;
		}
		if (start >= N_BUCKETS) {
			hashmap_area_release(the_hashmap, area);
			return NULL;
		}

		for (uint_fast32_t idx = start; idx < end; ++idx) {
			hashmap_key(&(idx), sizeof(idx), &(key));
			if (hashmap_cas(
				the_hashmap, area, &(key),
				&(x), NULL,
				hashmap_cas_get, NULL
			) != hashmap_cas_again) {
				puts("error!");
				exit(1);
			}
		}
	}
}

void *deletet(void *_) {
	while (nolock) hashmap_mpause();

	static atomic_uint_fast32_t chunk = 0;
	static const uint_fast32_t CHUNK_SZ = 1024;

	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);

	int total = 0;

	for (;;) {
		uint_fast32_t
			end = (chunk += CHUNK_SZ),
			start = end - CHUNK_SZ;
		if (end > N_BUCKETS) {
			end -= end - N_BUCKETS;
		}
		if (start >= N_BUCKETS) {
			hashmap_area_release(the_hashmap, area);
			return NULL;
		}

		for (uint_fast32_t idx = start; idx < end; ++idx) {
			hashmap_key(&(idx), sizeof(idx), &(key));
			if (hashmap_cas(
				the_hashmap, area, &(key),
				&(x), NULL + 1 /* anything but NULL */,
				hashmap_cas_delete, NULL
			) == hashmap_cas_error) {
				printf("error %lu\n", idx);
				exit(1);
			}
		}
	}
}

int main(int argc, char *argv[]) {
	the_hashmap = hashmap_create(
		N_THREADS, 25, 0.8,
		NULL
	);
	if (the_hashmap == NULL) {
		exit(1);
	}

	double time;
	pthread_t threads[N_THREADS];

	printf("writing %u values...\n", N_BUCKETS);
	for (size_t x = 0; x < N_THREADS; ++x) {
		pthread_create(&(threads[x]), NULL, (void *)&(writet), NULL);
	}
	time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\n", rc() - time);

	printf("reading %u values...\n", N_BUCKETS);
	for (size_t x = 0; x < N_THREADS; ++x) {
		pthread_create(&(threads[x]), NULL, (void *)&(readt), NULL);
	}
	//nolock = true;
	time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\n", rc() - time);

	printf("deleting %u values...\n", N_BUCKETS);
	for (size_t x = 0; x < N_THREADS; ++x) {
		pthread_create(&(threads[x]), NULL, (void *)&(deletet), NULL);
	}
	nolock = false;
	time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\n", rc() - time);

	hashmap_destroy(the_hashmap);

	return 0;
}