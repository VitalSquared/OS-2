#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define NUM_LINES 10

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
    char *line = (param == NULL) ? "" : (char *)param;
    size_t len = strlen(line);

    for(int i = 0; i < NUM_LINES; i++) {
        write(STDOUT_FILENO, line, len);
    }

    return param;
}

int main() {
    pthread_t thread;
    int code = pthread_create(&thread, NULL, print_lines, "Child\n");
    if (code != 0) {
        print_error("Unable to create thread", code);
        exit(EXIT_FAILURE);
    }

    print_lines("Parent\n");
    pthread_exit(NULL);
}
