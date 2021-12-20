#include <stdio.h>
#include <string.h>
#include "cache.h"
#include "states.h"

#define TRUE 1
#define FALSE 0

#define STR_EQ(STR1, STR2) (strcmp(STR1, STR2) == 0)

int cache_init(cache_t *cache) {
    int err_code = pthread_rwlock_init(&cache->rwlock, NULL);
    if (err_code != 0) {
        print_error("cache_init: Unable to init rwlock", err_code);
        return -1;
    }
    cache->head = NULL;
    return 0;
}

cache_entry_t *cache_add(char *host, char *path, char *data, ssize_t size, cache_t *cache) {
    cache_entry_t *node = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (node == NULL) {
        perror("cache_add: unable to allocate memory for cache entry");
        return NULL;
    }

    int err_code = pthread_rwlock_init(&node->rwlock, NULL);
    if (err_code != 0) {
        print_error("cache_add: Unable to init mutex", err_code);
        free(node);
        return NULL;
    }

    node->is_full = FALSE;
    node->size = size;
    node->data = data;
    node->host = host;
    node->path = path;

    write_lock_rwlock(&cache->rwlock, "cache_add: Unable to write-lock rwlock");
    node->prev = NULL;
    node->next = cache->head;
    cache->head = node;
    if (node->next != NULL) node->next->prev = node;
    unlock_rwlock(&cache->rwlock, "cache_add: Unable to unlock rwlock");

    return node;
}

cache_entry_t *cache_find(const char *host, const char *path, cache_t *cache) {
    read_lock_rwlock(&cache->rwlock, "cache_find: Unable to read-lock rwlock");
    cache_entry_t *cur = cache->head;
    while (cur != NULL) {
        if (STR_EQ(host, cur->host) && STR_EQ(path, cur->path)) break;
        cur = cur->next;
    }
    unlock_rwlock(&cache->rwlock, "cache_find: Unable to unlock rwlock");
    return cur;
}

void free_cache_entry(cache_entry_t *entry) {
    if (entry == NULL) return;
    free(entry->host);
    free(entry->path);
    free(entry->data);
    pthread_rwlock_destroy(&entry->rwlock);
    free(entry);
}

void cache_remove(cache_entry_t *entry, cache_t *cache) {
    write_lock_rwlock(&cache->rwlock, "cache_remove: Unable to write-lock rwlock");
    if (entry == cache->head) {
        cache->head = entry->next;
        if (cache->head != NULL) cache->head->prev = NULL;
    }
    else {
        entry->prev->next = entry->next;
        if (entry->next != NULL) entry->next->prev = entry->prev;
    }
    unlock_rwlock(&cache->rwlock, "cache_remove: Unable to unlock rwlock");
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
    pthread_rwlock_destroy(&cache->rwlock);
}

void cache_print_content(cache_t *cache) {
    read_lock_rwlock(&cache->rwlock, "cache_print_content: Unable to read-lock rwlock");
    cache_entry_t *cur = cache->head;
    while (cur != NULL) {
        printf("%s %s %zd full=%d\n", cur->host, cur->path, cur->size, cur->is_full);
        cur = cur->next;
    }
    unlock_rwlock(&cache->rwlock, "cache_print_content: Unable to unlock rwlock");
}
