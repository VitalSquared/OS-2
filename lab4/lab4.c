#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

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

void *print_lines(void *param) {
    while(1) {
        write(STDOUT_FILENO, "Child\n", 6);
    }
    return NULL;
}

void exact_sleep(unsigned time_left) {
    do {
        time_left = sleep(time_left);
    } while (time_left > 0);
}

int main() {
    pthread_t thread;
    int code = pthread_create(&thread, NULL, print_lines, NULL);
    if (code != 0) {
        print_error("Unable to create thread", code);
        exit(EXIT_FAILURE);
    }

    exact_sleep(2);

    const char *msg = "Parent: Trying to cancel child thread\n";
    write(STDOUT_FILENO, msg, strlen(msg));

    code = pthread_cancel(thread);
    if (code != 0) {
        print_error("Unable to cancel thread", code);
        exit(EXIT_FAILURE);
    }
    pthread_join(thread, NULL);

    msg = "Parent: Cancelled child thread\n";
    write(STDOUT_FILENO, msg, strlen(msg));

    pthread_exit(NULL);
}
