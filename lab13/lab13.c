#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUMBER_OF_LINES 10

#define MAIN_THREAD_ID 0
#define CHILD_THREAD_ID 1

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int cur_printing_thread = MAIN_THREAD_ID;

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

void lock_mutex(pthread_mutex_t *_mutex) {
    int error_code = pthread_mutex_lock(_mutex);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if mutex_lock fails
    }
}

void unlock_mutex(pthread_mutex_t *_mutex) {
    int error_code = pthread_mutex_unlock(_mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if mutex_lock fails
    }
}

void wait_cond(pthread_cond_t *_cond, pthread_mutex_t *_mutex) {
    int error_code = pthread_cond_wait(_cond, _mutex);
    if (error_code != 0) {
        print_error("Unable to wait cond variable", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if cond_wait fails
    }
}

void signal_cond(pthread_cond_t *_cond) {
    int error_code = pthread_cond_signal(_cond);
    if (error_code != 0) {
        print_error("Unable to signal cond", error_code);
        exit(EXIT_FAILURE); //we should stop the whole process if cond_signal fails
    }
}

void print_messages(const char *message, int calling_thread) {
    if (message == NULL) {
        fprintf(stderr, "print_messages: invalid parameter\n");
        return;
    }
    size_t msg_length = strlen(message);

    lock_mutex(&mutex);

    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        while (calling_thread != cur_printing_thread) {
            wait_cond(&cond, &mutex);
        }

        write(STDOUT_FILENO, message, msg_length);
        cur_printing_thread = (cur_printing_thread == MAIN_THREAD_ID) ? CHILD_THREAD_ID : MAIN_THREAD_ID;
        signal_cond(&cond);
    }

    unlock_mutex(&mutex);
}

void *second_print(void *param) {
    print_messages("Child\n", CHILD_THREAD_ID);
    return NULL;
}

int main() {
    pthread_t thread;
    int error_code = pthread_create(&thread, NULL, second_print, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        return EXIT_FAILURE;
    }

    print_messages("Parent\n", MAIN_THREAD_ID);
    pthread_exit(NULL);
}
