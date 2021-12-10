/*
 * This program generates files for 'remote.c' and 'clients.c' programs.
 *
 * Created by Vitaly Spirin, NSU, group 19203
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define BUF_SIZE 256
#define MAX_LINE_SIZE 64
#define MAX_NUM_OF_LINES 64
#define IS_NUM_OF_FILES_VALID(n) (0 < (n) && (n) <= 512)

void get_line(const char *prefix, char *buf, int file_index, int line_num, int total_lines) {
    sprintf(buf, "%s_%d.txt line %d out of %d\n", prefix, file_index, line_num + 1, total_lines);
}

void generate_file(const char *prefix, int index) {
    int num_of_lines = (rand() % MAX_NUM_OF_LINES) + 1;
    char name[BUF_SIZE] = { 0 };
    sprintf(name, "%s_%d.txt", prefix, index);
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd == -1) {
        perror(name);
        return;
    }

    for (int i = 0; i < num_of_lines; i++) {
        char line[MAX_LINE_SIZE + 1] = { 0 };
        get_line(prefix, line, index, i, num_of_lines);

        ssize_t bytes_written = write(fd, line, strlen(line));
        if (bytes_written == -1) {
            perror(name);
            close(fd);
            return;
        }
    }

    close(fd);
}

int convert_number(char *str, int *number) {
    errno = 0;
    char *endptr = "";
    long num = strtol(str, &endptr, 10);

    if (errno != 0) {
        perror("Can't convert given number");
        return -1;
    }
    if (strcmp(endptr, "") != 0) {
        fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }

    *number = (int)num;
    return 0;
}

int parse_args(char *num_of_files_str,  int *num_of_files) {
    if (convert_number(num_of_files_str, num_of_files) == -1) {
        return -1;
    }

    if (!IS_NUM_OF_FILES_VALID(*num_of_files)) {
        fprintf(stderr, "Number of files must be in range (0, 512]. Got: %d\n", *num_of_files);
        return EXIT_FAILURE;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s prefix num_of_files\n", argv[0]);
        return EXIT_SUCCESS;
    }

    char *prefix = argv[1];
    int num_of_files;
    if(parse_args(argv[2], &num_of_files) == -1) {
        return EXIT_FAILURE;
    }

    srand(time(NULL));
    for (int i = 0; i < num_of_files; i++) {
        generate_file(prefix, i);
    }

    return EXIT_SUCCESS;
}
