#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUMBER_OF_LINES 10

#define TRUE 1
#define FALSE 0

pthread_mutex_t mutexes[3] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
int thread_locked_mutex = FALSE;

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

void lock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_lock(mutex);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if mutex_lock fails
    }
}

void unlock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_unlock(mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if mutex_lock fails
    }
}

void *second_print(void *param) {
    lock_mutex(&mutexes[2]);
    thread_locked_mutex = TRUE; //we don't need mutex here since this operation is atomic
    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        lock_mutex(&mutexes[1]);
        write(STDOUT_FILENO, "Child\n", 6);
        unlock_mutex(&mutexes[2]);
        lock_mutex(&mutexes[0]);
        unlock_mutex(&mutexes[1]);
        lock_mutex(&mutexes[2]);
        unlock_mutex(&mutexes[1]);
    }
    unlock_mutex(&mutexes[2]);
    return NULL;
}

void first_print() {
    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        write(STDOUT_FILENO, "Parent\n", 7);
        lock_mutex(&mutexes[0]);
        unlock_mutex(&mutexes[1]);
        lock_mutex(&mutexes[2]);
        unlock_mutex(&mutexes[0]);
        lock_mutex(&mutexes[1]);
        unlock_mutex(&mutexes[2]);
    }
}

int main() {
    lock_mutex(&mutexes[1]);
    pthread_t thread;

    int error_code = pthread_create(&thread, NULL, second_print, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        return EXIT_FAILURE;
    }

    while (!thread_locked_mutex) {  //we don't need mutex here because other thread will modify 'thread_locked_mutex' atomically
        sched_yield();  //we let the other thread acquire mutexes[2]
    }

    first_print();
    unlock_mutex(&mutexes[1]);
    pthread_exit(NULL);
}
