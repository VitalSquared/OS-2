#include "http.h"
#include "cache.h"
#include "types.h"
#include "states.h"

#ifndef LAB33_CLIENT_H
#define LAB33_CLIENT_H

void create_client(int client_sock_fd, client_queue_t *client_queue);
void remove_client(client_t *client, client_list_t *client_list, client_list_t *global_client_list);

int client_init(client_t *client, int client_sock_fd);
void client_destroy(client_t *client);

void client_update_http_info(client_t *client);
void check_finished_writing_to_client(client_t *client);

void client_read_data(client_t *client, http_list_t *http_list, http_queue_t *http_queue, cache_t *cache);
void write_to_client(client_t *client);

#endif
