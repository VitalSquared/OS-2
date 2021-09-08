#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NO_ERROR 0
#define ERROR_BUF_SIZE 256

#define NUM_LINES 10
#define CHILD_TEXT "Child"
#define PARENT_TEXT "Parent"

#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

void print_error(const char *prefix_message, int error_code) {
    char error_message[ERROR_BUF_SIZE];
    strerror_r(error_code, error_message, ERROR_BUF_SIZE);
    if (!IS_STRING_EMPTY(prefix_message))
        fprintf(stderr, "%s: ", prefix_message);
    fprintf(stderr, "%s\n", error_message);
}

void *print_lines(void *param) {
    if (param == NULL) {
        fprintf(stderr, "print_lines: invalid param\n");
        return NULL;
    }

    char *line = (char *)param;
    for (int i = 0; i < NUM_LINES; i++)
        printf("%s\n", line);

    return NULL;
}

int main() {
    pthread_t thread;

    int error_code = pthread_create(&thread, NULL, print_lines, CHILD_TEXT);
    if (error_code != NO_ERROR) {
        print_error("Unable to create thread", error_code);
        return EXIT_FAILURE;
    }

    print_lines(PARENT_TEXT);
    pthread_exit(NULL);
}
