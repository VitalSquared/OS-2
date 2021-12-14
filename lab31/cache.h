#include <stdlib.h>

typedef struct cache_entry {
    int is_full;
    ssize_t size;
    char *data;
    char *host;
    char *path;

    struct cache_entry *next, *prev;
} cache_entry_t;

typedef struct {
    cache_entry_t *head;
} cache_t;

cache_entry_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache);

cache_entry_t *cache_find(const char *host, const char *path, cache_t *cache);

void cache_remove(cache_entry_t *entry, cache_t *cache);

void cache_destroy(cache_t *cache);

void cache_print_content(cache_t *cache);
