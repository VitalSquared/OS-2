#include <stdio.h>
#include <string.h>
#include "cache.h"

#define TRUE 1
#define FALSE 0

#define STR_EQ(STR1, STR2) (strcmp(STR1, STR2) == 0)

int cache_init(cache_t *cache) {
    cache->head = NULL;
    return 0;
}

cache_entry_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache) {
    cache_entry_t *node = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (node == NULL) {
        perror("cache_add: unable to allocate memory for cache entry");
        return NULL;
    }

    node->is_full = FALSE;
    node->size = size;
    node->data = data;
    node->host = host;
    node->path = path;

    node->prev = NULL;
    node->next = cache->head;
    cache->head = node;
    if (node->next != NULL) node->next->prev = node;

    return node;
}

cache_entry_t *cache_find(const char *host, const char *path, cache_t *cache) {
    cache_entry_t *cur = cache->head;
    while (cur != NULL) {
        if (STR_EQ(host, cur->host) && STR_EQ(path, cur->path)) break;
        cur = cur->next;
    }
    return cur;
}

void free_cache_entry(cache_entry_t *entry) {
    if (entry == NULL) return;
    free(entry->host);
    free(entry->path);
    free(entry->data);
    free(entry);
}

void cache_remove(cache_entry_t *entry, cache_t *cache) {
    if (entry == cache->head) {
        cache->head = entry->next;
        if (cache->head != NULL) cache->head->prev = NULL;
    }
    else {
        entry->prev->next = entry->next;
        if (entry->next != NULL) entry->next->prev = entry->prev;
    }

    free_cache_entry(entry);
}

void cache_destroy(cache_t *cache) {
    cache_entry_t *cur = cache->head;
    while (cur != NULL) {
        cache_entry_t *next = cur->next;
        free_cache_entry(cur);
        cur = next;
    }
    cache->head = NULL;
}

void cache_print_content(cache_t *cache) {
    cache_entry_t *cur = cache->head;
    while (cur != NULL) {
        printf("%s %s %zd full=%d\n", cur->host, cur->path, cur->size, cur->is_full);
        cur = cur->next;
    }
}
