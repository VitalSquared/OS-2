#include "list.h"

void http_add_to_list(http_t *http, http_list_t *http_list) {
    http->prev = NULL;
    http->next = http_list->head;
    http_list->head = http;
    if (http->next != NULL) http->next->prev = http;
}

void http_remove_from_list(http_t *http, http_list_t *http_list) {
    if (http == http_list->head) {
        http_list->head = http->next;
        if (http_list->head != NULL) http_list->head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (http->next != NULL) http->next->prev = http->prev;
    }
}

void client_add_to_list(client_t *client, client_list_t *client_list) {
    client->prev = NULL;
    client->next = client_list->head;
    client_list->head = client;
    if (client->next != NULL) client->next->prev = client;
}

void client_remove_from_list(client_t *client, client_list_t *client_list) {
    if (client == client_list->head) {
        client_list->head = client->next;
        if (client_list->head != NULL) client_list->head->prev = NULL;
    }
    else {
        client->prev->next = client->next;
        if (client->next != NULL) client->next->prev = client->prev;
    }
}
