#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUMBER_OF_LINES 10

#define TRUE 1
#define FALSE 0

pthread_mutex_t mutexes[3];
int child_thread_locked_mutex = FALSE;

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

void lock_mutex(int mutex_num) {
    int error_code = pthread_mutex_lock(&mutexes[mutex_num]);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
    }
}

void unlock_mutex(int mutex_num) {
    int error_code = pthread_mutex_unlock(&mutexes[mutex_num]);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
    }
}

void *second_print(void *param) {
    lock_mutex(2);
    child_thread_locked_mutex = TRUE;
    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        lock_mutex(1);
        write(STDOUT_FILENO, "Child\n", 6);
        unlock_mutex(2);
        lock_mutex(0);
        unlock_mutex(1);
        lock_mutex(2);
        unlock_mutex(0);
    }
    unlock_mutex(2);
    return NULL;
}

void first_print() {
    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        write(STDOUT_FILENO, "Parent\n", 7);
        lock_mutex(0);
        unlock_mutex(1);
        lock_mutex(2);
        unlock_mutex(0);
        lock_mutex(1);
        unlock_mutex(2);
    }
}

void destroy_mutexes(int count) {
    for (int i = 0; i < count; i++) {
        pthread_mutex_destroy(&mutexes[i]);
    }
}

int init_mutexes() {
    pthread_mutexattr_t mutex_attrs;
    int error_code = pthread_mutexattr_init(&mutex_attrs);
    if (error_code != 0) {
        print_error("Unable to init mutex attrs", error_code);
        return -1;
    }

    error_code = pthread_mutexattr_settype(&mutex_attrs, PTHREAD_MUTEX_ERRORCHECK);
    if (error_code != 0) {
        print_error("Unable to init mutex attrs type", error_code);
        pthread_mutexattr_destroy(&mutex_attrs);
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        error_code = pthread_mutex_init(&mutexes[i], &mutex_attrs);
        if (error_code != 0) {
            pthread_mutexattr_destroy(&mutex_attrs);
            destroy_mutexes(i);
            return -1;
        }
    }

    pthread_mutexattr_destroy(&mutex_attrs);
    return 0;
}

int main() {
    int error_code = init_mutexes();
    if (error_code != 0) {
        return EXIT_FAILURE;
    }

    lock_mutex(1);

    pthread_t thread;
    error_code = pthread_create(&thread, NULL, second_print, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        unlock_mutex(1);
        destroy_mutexes(3);
        return EXIT_FAILURE;
    }

    while (!child_thread_locked_mutex) {
        sched_yield();
    }

    first_print();
    unlock_mutex(1);

    destroy_mutexes(3);
    pthread_exit(NULL);
}
