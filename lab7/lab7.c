#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#define BUF_SIZE 4096
#define RETRY_SECONDS 5

#define STRINGS_EQUAL(STR1, STR2) (strcmp(STR1, STR2) == 0)
#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

typedef struct paths {
    char *src;
    char *dest;
} paths_t;

void *copy_path(void *param);

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

int open_file_with_retry(char *path, int oflag, mode_t mode) {
    while (1) {
        int fd = open(path, oflag, mode);
        if (fd != -1) {
            return fd;
        }
        if (errno != EMFILE) {
            perror(path);
            break;
        }
        sleep(RETRY_SECONDS);
    }
    return -1;
}

DIR *open_dir_with_retry(char *path) {
    while (1) {
        DIR *dir = opendir(path);
        if (dir != NULL) {
            return dir;
        }
        if (errno != EMFILE) {
            perror(path);
            break;
        }
        sleep(RETRY_SECONDS);
    }
    return NULL;
}

int create_thread_with_retry(void *param) {
    int error_code;
    pthread_t thread;
    while (1) {
        error_code = pthread_create(&thread, NULL, copy_path, param);
        if (error_code == 0) {
            pthread_detach(thread);
            break;
        }
        if (error_code != EAGAIN) {
            print_error("Unable to create thread", error_code);
            break;
        }
        sleep(RETRY_SECONDS);
    }
    return error_code;
}

void *malloc_with_retry(size_t size) {
    while (1) {
        void *ptr = malloc(size);
        if (ptr != NULL) {
            memset(ptr, 0, size);
            return ptr;
        }
        if (errno != EAGAIN) {
            perror("Unable to allocate memory");
            break;
        }
        sleep(RETRY_SECONDS);
    }
    return NULL;
}

char *concat_strings(const char **strings) {
    if (strings == NULL) {
        fprintf(stderr, "concat_strings: invalid strings\n");
        return NULL;
    }

    size_t total_length = 0;
    for (int i = 0; strings[i] != NULL; i++) {
        total_length += strlen(strings[i]);
    }

    char *str = (char *)malloc_with_retry((total_length + 1) * sizeof(char));

    if (str != NULL) {
        for (int i = 0; strings[i] != NULL; i++) {
            strcat(str, strings[i]);
        }
    }

    return str;
}

void free_paths(paths_t *paths) {
    if (paths == NULL) {
        return;
    }

    free(paths->src);
    free(paths->dest);
    free(paths);
}

paths_t *build_paths(const char *src_path, const char *dest_path, const char *sub_path) {
    if (src_path == NULL || dest_path == NULL) {
        fprintf(stderr, "build_paths: invalid src_path and/or dest_path\n");
        return NULL;
    }

    paths_t *paths = (paths_t *)malloc_with_retry(sizeof(paths_t));
    if (paths == NULL) {
        return NULL;
    }

    char *delimiter = IS_STRING_EMPTY(sub_path) ? "" : "/";
	sub_path = IS_STRING_EMPTY(sub_path) ? "" : sub_path;

    paths->src = concat_strings((const char *[]){ src_path, delimiter, sub_path, NULL });
    paths->dest = concat_strings((const char *[]){ dest_path, delimiter, sub_path, NULL });

    if (paths->src == NULL || paths->dest == NULL) {
        free_paths(paths);
        return NULL;
    }

    return paths;
}

void traverse_directory(DIR *dir, struct dirent *entry_buf, paths_t *old_paths) {
    if (old_paths == NULL) {
        fprintf(stderr, "traverse_directory: invalid paths\n");
        return;
    }

    int error_code;
    struct dirent *result;

    while (1) {
        error_code = readdir_r(dir, entry_buf, &result);
        if (error_code != 0) {
            print_error(old_paths->src, error_code);
            break;
        }
        if (result == NULL) {
            break;
        }

        if (STRINGS_EQUAL(entry_buf->d_name, ".") || STRINGS_EQUAL(entry_buf->d_name, "..")) {
            continue;
        }

        paths_t *new_paths = build_paths(old_paths->src, old_paths->dest, entry_buf->d_name);
        if (new_paths == NULL) {
            continue;
        }

        if (create_thread_with_retry(new_paths) != 0) {
            free_paths(new_paths);
            continue;
        }
    }
}

void copy_directory(paths_t *paths, mode_t mode) {
    if (paths == NULL || paths->src == NULL || paths->dest == NULL) {
        fprintf(stderr, "copy_directory: invalid paths\n");
        return;
    }

    errno = 0;  //errno is unique to calling thread
    if (mkdir(paths->dest, mode) == -1 && errno != EEXIST) {
        perror(paths->dest);
        return;
    }

    DIR *dir = open_dir_with_retry(paths->src);
    if (dir == NULL) {
        return;
    }

    struct dirent *entry = (struct dirent *)malloc_with_retry(sizeof(struct dirent) + pathconf(paths->src, _PC_NAME_MAX) + 1);  // allocate extra memory for d_name
    if (entry == NULL) {
        closedir(dir);
        return;
    }

    traverse_directory(dir, entry, paths);

    free(entry);
    if (closedir(dir) == -1) {
        perror(paths->src);
    }
}

void copy_file_content(int src_fd, int dest_fd, paths_t *paths) {
    if (paths == NULL) {
        fprintf(stderr, "copy_file_content: invalid paths\n");
        return;
    }

    char buf[BUF_SIZE];
    while (1) {
        ssize_t bytes_read = read(src_fd, buf, BUF_SIZE);
        if (bytes_read == -1) {
            perror(paths->src);
            return;
        }
        if (bytes_read == 0) {
            break;
        }

        ssize_t offset = 0;
        ssize_t bytes_written;
        while (offset < bytes_read) {
            bytes_written = write(dest_fd, buf + offset, bytes_read - offset);
            if (bytes_written == -1) {
                perror(paths->dest);
                return;
            }
            offset += bytes_written;
        }
    }
}

void copy_regular_file(paths_t *paths, mode_t mode) {
    if (paths == NULL || paths->src == NULL || paths->dest == NULL) {
        fprintf(stderr, "copy_regular_file: invalid paths\n");
        return;
    }

    int src_fd = open_file_with_retry(paths->src, O_RDONLY, mode);
    if (src_fd == -1) {
        return;
    }

    int dest_fd = open_file_with_retry(paths->dest, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (dest_fd == -1) {
        close(src_fd);
        return;
    }

    copy_file_content(src_fd, dest_fd, paths);

    if (close(src_fd) == -1) {
        perror(paths->src);
    }
    if (close(dest_fd) == -1) {
        perror(paths->dest);
    }
}

void *copy_path(void *param) {
    if (param == NULL) {
        fprintf(stderr, "copy_path: invalid param\n");
        return NULL;
    }

	paths_t *paths = (paths_t *)param;
    struct stat stat_buf;
	
    if (lstat(paths->src, &stat_buf) == -1) {
        perror(paths->src);
        free_paths(paths);
        return NULL;
    }

    if (S_ISDIR(stat_buf.st_mode)) {
        copy_directory(paths, stat_buf.st_mode);
    }
    else if (S_ISREG(stat_buf.st_mode)) {
        copy_regular_file(paths, stat_buf.st_mode);
    }

    free_paths(paths);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s src_path dest_path\n", argv[0]);
        return EXIT_SUCCESS;
    }

    paths_t *paths = build_paths(argv[1], argv[2], "");
    if (paths == NULL) {
        return EXIT_FAILURE;
    }

    copy_path(paths);  // we don't spawn thread for src_path, but call this method in main thread
    pthread_exit(NULL);
}
