#include <stdlib.h>
#include <semaphore.h>

typedef struct node {
    struct node *prev;
    struct node *next;
    char value[81];
} node_t;

typedef struct queue {
    int dropped;

    sem_t head_sem;
    sem_t tail_sem;
    sem_t queue_sem;

    node_t *head;
    node_t *tail;
} queue_t;

/**
 * Inits queue.
 */
void mymsginit(queue_t *queue);

/**
 * Unblocks mymsgput and mymsgget.
 */
void mymsgdrop(queue_t *queue);

/**
 * Frees memory of queue.
 */
void mymsgdestroy(queue_t *queue);

/**
 * Accepts a string, which is truncated to 80 symbols if long, and puts it in the queue. If queue has more than 10 entries, mymsgput blocks.
 * @return amount of bytes transferred on success (including '\0').
 * @return 0 if mymsgput was blocked and mymsgdrop was called.
 */
int mymsgput(queue_t *queue, char *buf);

/**
 * Removes first entry of queue and copies buf_sz bytes at most in the buffer. If queue is empty, mymsgget blocks.
 * @return amount of bytes transferred on success (including '\0').
 * @return 0 if mymsgget was blocked and mymsgdrop was called.
 */
int mymsgget(queue_t *queue, char *buf, size_t buf_sz);
