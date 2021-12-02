#include "msgqueue_sem.h"
#include <string.h>
#include <stdio.h>

#define TRUE 1
#define FALSE 0

void mymsginit(queue_t *queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->dropped = FALSE;

    sem_init(&queue->head_sem, 0, 10);
    sem_init(&queue->tail_sem, 0, 0);
    sem_init(&queue->queue_sem, 0, 1);
}

void mymsgdrop(queue_t *queue) {
    queue->dropped = TRUE;
    sem_wait(&queue->queue_sem);
    sem_post(&queue->head_sem);
    sem_post(&queue->tail_sem);
    sem_post(&queue->queue_sem);
}

void mymsgdestroy(queue_t *queue) {
    node_t *cur = queue->head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free(cur);
        cur = next;
    }
    sem_destroy(&queue->head_sem);
    sem_destroy(&queue->tail_sem);
    sem_destroy(&queue->queue_sem);
}

int mymsgput(queue_t *queue, char *msg) {
    sem_wait(&queue->head_sem);
    sem_wait(&queue->queue_sem);

    if (queue->dropped) {
        sem_post(&queue->head_sem);
        sem_post(&queue->queue_sem);
        return 0;
    }

    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (new_node == NULL) {
        perror("Unable to allocate memory");
        sem_post(&queue->queue_sem);
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

    sem_post(&queue->queue_sem);
    sem_post(&queue->tail_sem);

    return strlen(new_node->value) + 1;
}

int mymsgget(queue_t *queue, char *buf, size_t buf_sz) {
    sem_wait(&queue->tail_sem);
    sem_wait(&queue->queue_sem);

    if (queue->dropped) {
        sem_post(&queue->tail_sem);
        sem_post(&queue->queue_sem);
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

    sem_post(&queue->queue_sem);

    strncpy(buf, tail->value, buf_sz - 1);
    buf[buf_sz - 1] = '\0';
    free(tail);

    sem_post(&queue->head_sem);

    return strlen(buf) + 1;
}
