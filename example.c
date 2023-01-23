#define HASHMAP_HASH_FUNCTION(key) (*(uint64_t *)key ^ 9268326398 /* arbitrary integer */)
#include "hashmap.h"
#include <time.h>
#include <stdio.h>

#define N_THREADS 16
#define _N_BUCKETS 24000000
#define N_BUCKETS (size_t)((_N_BUCKETS / N_THREADS) * N_THREADS)

struct hashmap *the_hashmap;
struct gosh {
	size_t idx;
	size_t n;
};

void *go(struct gosh *gosh) {
	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);
	hashmap_reserve(the_hashmap, area, gosh->n);

	for (size_t it = 0; it < gosh->n; ++it) {
		size_t idx = gosh->idx + it;

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

	hashmap_area_release(the_hashmap, area);
	return NULL;
}

void *readt(struct gosh *gosh) {
	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);

	for (size_t it = 0; it < gosh->n; ++it) {
		size_t idx = gosh->idx + it;

		hashmap_key(&(idx), sizeof(idx), &(key));
		if (hashmap_cas(
			the_hashmap, area, &(key),
			&(x), NULL,
			hashmap_cas_get, NULL
		) == hashmap_cas_error) {
			puts("error!");
			exit(1);
		}
	}

	hashmap_area_release(the_hashmap, area);
	return NULL;
}

void *deletet(struct gosh *gosh) {
	void *x;

	struct hashmap_key key;

	struct hashmap_area *area = hashmap_area(the_hashmap);

	for (size_t it = 0; it < gosh->n; ++it) {
		size_t idx = gosh->idx + it;

		hashmap_key(&(idx), sizeof(idx), &(key));
		if (hashmap_cas(
			the_hashmap, area, &(key),
			&(x), NULL + 1,
			hashmap_cas_delete, NULL
		) == hashmap_cas_error) {
			puts("error!");
			exit(1);
		}
	}

	hashmap_area_release(the_hashmap, area);
	return NULL;
}

double rc(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &(now));
    return now.tv_sec + (now.tv_nsec * 1e-9);
}

int main(int argc, char *argv[]) {
	the_hashmap = hashmap_create(
		N_THREADS, N_BUCKETS / 0.94, 1, 2,
		NULL
	);
	if (the_hashmap == NULL) {
		puts("error");
		exit(1);
	}

	printf("writing %zu values...\n", N_BUCKETS);

	pthread_t threads[N_THREADS];
	struct gosh gosh[N_THREADS];
	size_t idx = 0;
	for (size_t x = 0; x < N_THREADS; ++x) {
		gosh[x].idx = idx;
		idx += (gosh[x].n = (N_BUCKETS / N_THREADS));
		pthread_create(&(threads[x]), NULL, (void *)&(go), &(gosh[x]));
	}

	double time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\nreading %zu values...\n", rc() - time, N_BUCKETS);

	for (size_t x = 0; x < N_THREADS; ++x) {
		pthread_create(&(threads[x]), NULL, (void *)&(readt), &(gosh[x]));
	}

	time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\n", rc() - time);

	for (size_t x = 0; x < N_THREADS; ++x) {
		pthread_create(&(threads[x]), NULL, (void *)&(deletet), &(gosh[x]));
	}

	time = rc();
	for (size_t x = 0; x < N_THREADS; ++x) {
		void *fuck;
		pthread_join(threads[x], &(fuck));
	}
	printf("success! %lfs\n", rc() - time);

	hashmap_destroy(the_hashmap);

	return 0;
}