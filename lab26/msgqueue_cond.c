#include "msgqueue_cond.h"
#include <string.h>
#include <stdio.h>

#define TRUE 1
#define FALSE 0

void mymsginit(queue_t *queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->dropped = FALSE;
    queue->count = 0;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}

void mymsgdrop(queue_t *queue) {
    queue->dropped = TRUE;
    pthread_cond_broadcast(&queue->cond);
}

void mymsgdestroy(queue_t *queue) {
    node_t *cur = queue->head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free(cur);
        cur = next;
    }
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
}

int mymsgput(queue_t *queue, char *msg) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count >= 10 && !queue->dropped) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->dropped) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (new_node == NULL) {
        perror("Unable to allocate memory");
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    strncpy(new_node->value, msg, 80);
    new_node->value[80] = '\0';
    new_node->prev = NULL;
    new_node->next = queue->head;

    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = new_node;
    }
    else {
        queue->head->prev = new_node;
        queue->head = new_node;
    }

    if (queue->count == 0) {
        pthread_cond_broadcast(&queue->cond);
    }
    queue->count++;
    pthread_mutex_unlock(&queue->mutex);

    return strlen(new_node->value) + 1;
}

int mymsgget(queue_t *queue, char *buf, size_t buf_sz) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->dropped) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }

    if (queue->dropped) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    node_t *tail = queue->tail;

    if (queue->head == queue->tail) {
        queue->head = NULL;
        queue->tail = NULL;
    }
    else {
        queue->tail = queue->tail->prev;
        queue->tail->next = NULL;
    }

    if (queue->count == 10) {
        pthread_cond_broadcast(&queue->cond);
    }
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);

    strncpy(buf, tail->value, buf_sz - 1);
    buf[buf_sz - 1] = '\0';
    free(tail);

    return strlen(buf) + 1;
}
