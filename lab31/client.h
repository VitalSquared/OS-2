#include "http.h"
#include "cache.h"
#include "types.h"
#include "states.h"

#ifndef LAB31_CLIENT_H
#define LAB31_CLIENT_H
void create_client(int client_sock_fd, client_list_t *client_list);
void remove_client(client_t *client, client_list_t *client_list);

int client_init(client_t *client, int client_sock_fd);
void client_destroy(client_t *client);

void client_update_http_info(client_t *client);
void check_finished_writing_to_client(client_t *client);

void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache);
void write_to_client(client_t *client);

#endif
