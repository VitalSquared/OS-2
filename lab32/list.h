#include "types.h"

#ifndef LAB32_LIST_H
#define LAB32_LIST_H

void http_add_to_list(http_t *http, http_list_t *http_list);
void http_remove_from_list(http_t *http, http_list_t *http_list);

void client_add_to_list(client_t *client, client_list_t *client_list);
void client_remove_from_list(client_t *client, client_list_t *client_list);

#endif
