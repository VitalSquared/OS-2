#include <unistd.h>
#include "list_queue.h"
#include "states.h"

void http_add_to_global_list(http_t *http, http_list_t *http_list) {
    write_lock_rwlock(&http_list->rwlock, "http_add_to_list");
    http->global_prev = NULL;
    http->global_next = http_list->head;
    http_list->head = http;
    if (http->global_next != NULL) http->global_next->global_prev = http;
    http_list->size++;
    unlock_rwlock(&http_list->rwlock, "http_add_to_list");
}

void http_remove_from_global_list(http_t *http, http_list_t *http_list) {
    write_lock_rwlock(&http_list->rwlock, "http_remove_from_list");
    if (http == http_list->head) {
        http_list->head = http->global_next;
        if (http_list->head != NULL) http_list->head->global_prev = NULL;
    }
    else {
        http->global_prev->global_next = http->global_next;
        if (http->global_next != NULL) http->global_next->global_prev = http->global_prev;
    }
    http_list->size--;
    unlock_rwlock(&http_list->rwlock, "http_remove_from_list");
}

void client_add_to_global_list(client_t *client, client_list_t *client_list) {
    write_lock_rwlock(&client_list->rwlock, "client_add_to_list");
    client->global_prev = NULL;
    client->global_next = client_list->head;
    client_list->head = client;
    if (client->global_next != NULL) client->global_next->global_prev = client;
    client_list->size++;
    unlock_rwlock(&client_list->rwlock, "client_add_to_list");
}

void client_remove_from_global_list(client_t *client, client_list_t *client_list) {
    write_lock_rwlock(&client_list->rwlock, "client_remove_from_list");
    if (client == client_list->head) {
        client_list->head = client->global_next;
        if (client_list->head != NULL) client_list->head->global_prev = NULL;
    }
    else {
        client->global_prev->global_next = client->global_next;
        if (client->global_next != NULL) client->global_next->global_prev = client->global_prev;
    }
    client_list->size--;
    unlock_rwlock(&client_list->rwlock, "client_remove_from_list");
}

void http_add_to_list(http_t *http, http_list_t *http_list) {
    write_lock_rwlock(&http_list->rwlock, "http_add_to_list");
    http->prev = NULL;
    http->next = http_list->head;
    http_list->head = http;
    if (http->next != NULL) http->next->prev = http;
    http_list->size++;
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
    http_list->size--;
    unlock_rwlock(&http_list->rwlock, "http_remove_from_list");
}

void client_add_to_list(client_t *client, client_list_t *client_list) {
    write_lock_rwlock(&client_list->rwlock, "client_add_to_list");
    client->prev = NULL;
    client->next = client_list->head;
    client_list->head = client;
    if (client->next != NULL) client->next->prev = client;
    client_list->size++;
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
    client_list->size--;
    unlock_rwlock(&client_list->rwlock, "client_remove_from_list");
}

void http_enqueue(http_t *http, http_queue_t *http_queue) {
    pthread_mutex_lock(&http_queue->mutex);

    http->prev = NULL;
    http->next = http_queue->head;
    http_queue->head = http;
    if (http->next != NULL) http->next->prev = http;
    if (http_queue->tail == NULL) http_queue->tail = http_queue->head;

    pthread_cond_broadcast(&http_queue->cond);

    char buf1[1] = { 1 };
    write(http_queue->wakeup_pipe_fd, buf1, 1);

    pthread_mutex_unlock(&http_queue->mutex);
}

http_t *http_dequeue(http_queue_t *http_queue, int num) {
    http_t *new_http = NULL;
    pthread_mutex_lock(&http_queue->mutex);

    while (http_queue->head == NULL) {
        if (num != 0) break;
        pthread_cond_wait(&http_queue->cond, &http_queue->mutex);
    }

    if (http_queue->head != NULL && num <= http_queue->max_num) {
        new_http = http_queue->tail;
        http_queue->tail = http_queue->tail->prev;
        if (http_queue->tail != NULL) http_queue->tail->next = NULL;
        else http_queue->head = NULL;
    }
    else if (http_queue->head != NULL) {
        char buf1[1] = { 1 };
        write(http_queue->wakeup_pipe_fd, buf1, 1);
        pthread_cond_broadcast(&http_queue->cond);
    }

    pthread_mutex_unlock(&http_queue->mutex);
    return new_http;
}

void client_enqueue(client_t *client, client_queue_t *client_queue) {
    pthread_mutex_lock(&client_queue->mutex);

    client->prev = NULL;
    client->next = client_queue->head;
    client_queue->head = client;
    if (client->next != NULL) client->next->prev = client;
    if (client_queue->tail == NULL) client_queue->tail = client_queue->head;

    pthread_cond_broadcast(&client_queue->cond);

    char buf1[1] = { 1 };
    write(client_queue->wakeup_pipe_fd, buf1, 1);

    pthread_mutex_unlock(&client_queue->mutex);
}

client_t *client_dequeue(client_queue_t *client_queue, int num) {
    client_t *new_client = NULL;
    pthread_mutex_lock(&client_queue->mutex);

    while (client_queue->head == NULL) {
        if (num != 0) break;
        pthread_cond_wait(&client_queue->cond, &client_queue->mutex);
    }

    if (client_queue->head != NULL && num <= client_queue->max_num) {
        new_client = client_queue->tail;
        client_queue->tail = client_queue->tail->prev;
        if (client_queue->tail != NULL) client_queue->tail->next = NULL;
        else client_queue->head = NULL;
    }
    else if (client_queue->head != NULL) {
        char buf1[1] = { 1 };
        write(client_queue->wakeup_pipe_fd, buf1, 1);
        pthread_cond_broadcast(&client_queue->cond);
    }

    pthread_mutex_unlock(&client_queue->mutex);
    return new_client;
}
