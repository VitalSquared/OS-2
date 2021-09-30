#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <unistd.h>

#define NUMBER_OF_LINES 10

#define TRUE 1
#define FALSE 0

sem_t semaphores[2];

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

void wait_semaphore(sem_t *sem) {
    if (sem_wait(sem) == -1) {
        perror("Unable to wait semaphore");
        exit(EXIT_FAILURE); //we should stop the whole process if sem_wait fails
    }
}

void post_semaphore(sem_t *sem) {
    if (sem_post(sem) == -1) {
        perror("Unable to wait semaphore");
        exit(EXIT_FAILURE); //we should stop the whole process if sem_wait fails
    }
}

void print_messages(int first_sem, int second_sem, const char *message) {
    if (message == NULL) {
        fprintf(stderr, "print_messages: message was null");
        return;
    }
    size_t msg_length = strlen(message);

    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        wait_semaphore(&semaphores[first_sem]);
        write(STDOUT_FILENO, message, msg_length);
        post_semaphore(&semaphores[second_sem]);
    }
}

void *second_print(void *param) {
    print_messages(1, 0, "Child\n");
    return NULL;
}

int main() {
    int error_code = sem_init(&semaphores[0], 0, 1);
    if (error_code == -1) {
        perror("Unable to init semaphore");
        return EXIT_FAILURE;
    }

    error_code = sem_init(&semaphores[1], 0, 0);
    if (error_code == -1) {
        perror("Unable to init semaphore");
        sem_destroy(&semaphores[0]);
        return EXIT_FAILURE;
    }

    pthread_t thread;
    error_code = pthread_create(&thread, NULL, second_print, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        sem_destroy(&semaphores[0]);
        sem_destroy(&semaphores[1]);
        return EXIT_FAILURE;
    }

    print_messages(0, 1, "Parent\n");

    error_code = pthread_join(thread, NULL);
    if (error_code != 0) {
        print_error("Unable to join thread", error_code);
    }

    sem_destroy(&semaphores[0]);
    sem_destroy(&semaphores[1]);
    pthread_exit(NULL);
}
