#include "list.h"
#include "states.h"

void http_add_to_list(http_t *http, http_list_t *http_list) {
    write_lock_rwlock(&http_list->rwlock, "http_add_to_list");
    http->prev = NULL;
    http->next = http_list->head;
    http_list->head = http;
    if (http->next != NULL) http->next->prev = http;
    unlock_rwlock(&http_list->rwlock, "http_add_to_list");
}

void http_remove_from_list(http_t *http, http_list_t *http_list) {
    write_lock_rwlock(&http_list->rwlock, "http_remove_from_list");
    if (http == http_list->head) {
        http_list->head = http->next;
        if (http_list->head != NULL) http_list->head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (http->next != NULL) http->next->prev = http->prev;
    }
    unlock_rwlock(&http_list->rwlock, "http_remove_from_list");
}

void client_add_to_list(client_t *client, client_list_t *client_list) {
    write_lock_rwlock(&client_list->rwlock, "client_add_to_list");
    client->prev = NULL;
    client->next = client_list->head;
    client_list->head = client;
    if (client->next != NULL) client->next->prev = client;
    unlock_rwlock(&client_list->rwlock, "client_add_to_list");
}

void client_remove_from_list(client_t *client, client_list_t *client_list) {
    write_lock_rwlock(&client_list->rwlock, "client_remove_from_list");
    if (client == client_list->head) {
        client_list->head = client->next;
        if (client_list->head != NULL) client_list->head->prev = NULL;
    }
    else {
        client->prev->next = client->next;
        if (client->next != NULL) client->next->prev = client->prev;
    }
    unlock_rwlock(&client_list->rwlock, "client_remove_from_list");
}
