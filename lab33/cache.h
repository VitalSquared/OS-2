#include <stdlib.h>
#include <pthread.h>

#ifndef LAB33_CACHE_H
#define LAB33_CACHE_H

typedef struct cache_entry {
    int is_full;
    char *data; ssize_t size;
    char *host, *path;
    pthread_rwlock_t rwlock;
    struct cache_entry *next, *prev;
} cache_entry_t;

typedef struct {
    cache_entry_t *head;
    pthread_rwlock_t rwlock;
} cache_t;

int cache_init(cache_t *cache);

cache_entry_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache);
cache_entry_t *cache_find(const char *host, const char *path, cache_t *cache);
void cache_remove(cache_entry_t *entry, cache_t *cache);
void cache_destroy(cache_t *cache);
void cache_print_content(cache_t *cache);

#endif
