#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "msgqueue_sem.h"

#define NUMBER_OF_PRODUCERS 2
#define NUMBER_OF_CONSUMERS 2
#define PRODUCER_NUM_MESSAGES 100

#define TRUE 1
#define FALSE 0

queue_t queue;

void print_error(const char *prefix, int code) {
    if (prefix == NULL) {
        prefix = "error";
    }
    char buf[256];
    if (strerror_r(code, buf, sizeof(buf)) != 0) {
        strcpy(buf, "(unable to generate error!)");
    }
    fprintf(stderr, "%s: %s\n", prefix, buf);
}

void *producer(void *param) {
    char buf[81];
    for (int i = 0; i < PRODUCER_NUM_MESSAGES; i++) {
        sprintf(buf, "Message %d from thread %d", i, pthread_self());
        int bytes_transferred = mymsgput(&queue, buf);
        if (bytes_transferred == 0) {
            break;
        }
    }
    return param;
}

void *consumer(void *param) {
    char buf[81];
    do {
        int bytes_transferred = mymsgget(&queue, buf, sizeof(buf));
        if (bytes_transferred == 0) {
            break;
        }
        printf("Received by thread %d: %s\n", pthread_self(), buf);
    } while (1);
    return NULL;
}

int create_threads(pthread_t *threads, int number_of_threads_requested, void *(*thread_func)(void *)) {
    int error_code;
    int num_of_created = 0;
    for (int i = 0; i < number_of_threads_requested; i++) {
        error_code = pthread_create(&threads[i], NULL, thread_func, NULL);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            break;
        }
        num_of_created++;
    }
    return num_of_created;
}

void join_threads(pthread_t *threads, int number_of_threads) {
    int error_code;
    for (int i = 0; i < number_of_threads; i++) {
        error_code = pthread_join(threads[i], NULL);
        if (error_code != 0) {
            print_error("Unable to join thread", error_code);
        }
    }
}

int main() {
    pthread_t producers[NUMBER_OF_PRODUCERS];
    pthread_t consumers[NUMBER_OF_CONSUMERS];

    mymsginit(&queue);

    int number_of_producers_spawned = create_threads(producers, NUMBER_OF_PRODUCERS, producer);
    int number_of_consumers_spawned = create_threads(consumers, NUMBER_OF_CONSUMERS, consumer);

    sleep(10);

    mymsgdrop(&queue);
    join_threads(producers, number_of_producers_spawned);
    join_threads(consumers, number_of_consumers_spawned);
    mymsgdestroy(&queue);

    return EXIT_SUCCESS;
}
