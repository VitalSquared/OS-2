#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

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
    char **lines = (char **)param;
    for(char **line = lines; *line != NULL; line++) {
        write(STDOUT_FILENO, *line, strlen(*line));
    }
    return NULL;
}

void create_thread(char **lines) {
    pthread_t thread;
    int code = pthread_create(&thread, NULL, print_lines, lines);
    if (code != 0) {
        print_error("Unable to create thread", code);
    }
}

int main() {
    static char *lines_1[] = { "[1] line 1\n", "[1] line 2\n", "[1] line 3\n", NULL };
    static char *lines_2[] = { "[2] line 1\n", "[2] line 2\n", "[2] line 3\n", "[2] line 4\n", NULL };
    static char *lines_3[] = { "[3] line 1\n", "[3] line 2\n", "[3] line 3\n", NULL };
    static char *lines_4[] = { "[4] line 1\n", "[4] line 2\n", NULL };

    create_thread(lines_1);
    create_thread(lines_2);
    create_thread(lines_3);
    create_thread(lines_4);

    write(STDOUT_FILENO, "Parent\n", 7);
    pthread_exit(NULL);
}
