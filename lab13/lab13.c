#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUMBER_OF_LINES 10

#define MAIN_THREAD 0
#define CHILD_THREAD 1

pthread_mutex_t mutex;
pthread_cond_t cond;
int cur_printing_thread = MAIN_THREAD;

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
    }
}

void unlock_mutex(pthread_mutex_t *_mutex) {
    int error_code = pthread_mutex_unlock(_mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
    }
}

void wait_cond(pthread_cond_t *_cond, pthread_mutex_t *_mutex) {
    int error_code = pthread_cond_wait(_cond, _mutex);
    if (error_code != 0) {
        print_error("Unable to wait cond variable", error_code);
    }
}

void signal_cond(pthread_cond_t *_cond) {
    int error_code = pthread_cond_signal(_cond);
    if (error_code != 0) {
        print_error("Unable to signal cond", error_code);
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
        cur_printing_thread = (cur_printing_thread == MAIN_THREAD) ? CHILD_THREAD : MAIN_THREAD;
        signal_cond(&cond);
    }

    unlock_mutex(&mutex);
}

void *second_print(void *param) {
    print_messages("Child\n", CHILD_THREAD);
    return NULL;
}

int init() {
    int error_code = pthread_mutex_init(&mutex, NULL);
    if (error_code != 0) {
        print_error("Unable to init mutex", error_code);
        return -1;
    }

    error_code = pthread_cond_init(&cond, NULL);
    if (error_code != 0) {
        print_error("Unable to init cond", error_code);
        pthread_mutex_destroy(&mutex);
        return -1;
    }

    return 0;
}

void cleanup() {
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
}

int main() {
    if (init() != 0) {
        return EXIT_FAILURE;
    }

    pthread_t thread;
    int error_code = pthread_create(&thread, NULL, second_print, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        cleanup();
        return EXIT_FAILURE;
    }

    print_messages("Parent\n", MAIN_THREAD);

    cleanup();
    pthread_exit(NULL);
}
