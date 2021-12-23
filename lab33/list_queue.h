#include "types.h"

#ifndef LAB33_LIST_H
#define LAB33_LIST_H

void http_add_to_global_list(http_t *http, http_list_t *http_list);
void http_remove_from_global_list(http_t *http, http_list_t *http_list);

void client_add_to_global_list(client_t *client, client_list_t *client_list);
void client_remove_from_global_list(client_t *client, client_list_t *client_list);

void http_add_to_list(http_t *http, http_list_t *http_list);
void http_remove_from_list(http_t *http, http_list_t *http_list);

void client_add_to_list(client_t *client, client_list_t *client_list);
void client_remove_from_list(client_t *client, client_list_t *client_list);

void http_enqueue(http_t *http, http_queue_t *http_queue);
http_t *http_dequeue(http_queue_t *http_queue, int num);

void client_enqueue(client_t *client, client_queue_t *client_queue);
client_t *client_dequeue(client_queue_t *client_queue, int num);

#endif
