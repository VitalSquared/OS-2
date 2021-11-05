#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define NUMBER_OF_SORTING_THREADS 1
#define MID_SORT_WAIT_SECONDS 1

#define BUF_SIZE 80

#define TRUE 1
#define FALSE 0

#define STR_ORDERED(STR1, STR2) (strcmp(STR1, STR2) <= 0)

typedef struct node {
    pthread_rwlock_t rwlock;
    char *value;
    struct node *next;
} node_t;

node_t *global_list_head = NULL;

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

int read_lock_node_rwlock(node_t *node) {
    if (node == NULL) {
        return 0;
    }

    int error_code = pthread_rwlock_rdlock(&node->rwlock);
    if (error_code != 0) {
        print_error("Unable to read-lock rwlock", error_code);
    }
    return error_code;
}

int write_lock_node_rwlock(node_t *node) {
    if (node == NULL) {
        return 0;
    }

    int error_code = pthread_rwlock_wrlock(&node->rwlock);
    if (error_code != 0) {
        print_error("Unable to write-lock rwlock", error_code);
    }
    return error_code;
}

int unlock_node_rwlock(node_t *node) {
    if (node == NULL) {
        return 0;
    }

    int error_code = pthread_rwlock_unlock(&node->rwlock);
    if (error_code != 0) {
        print_error("Unable to unlock rwlock", error_code);
    }
    return error_code;
}

node_t *list_create() {
    node_t *head = (node_t *)malloc(sizeof(node_t));
    if (head == NULL) {
        perror("Unable to allocate memory for head");
        return NULL;
    }

    int error_code = pthread_rwlock_init(&head->rwlock, NULL);
    if (error_code != 0) {
        free(head);
        print_error("Unable to init head rwlock", error_code);
        return NULL;
    }

    head->next = NULL;
    head->value = NULL;

    return head;
}

void free_node(node_t *node) {
    if (node != NULL) {
        pthread_rwlock_destroy(&node->rwlock);
        free(node->value);
        free(node);
    }
}

void list_destroy(node_t *head) {
    if (head == NULL) {
        return;
    }

    node_t *cur = head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free_node(cur);
        cur = next;
    }
}

int list_insert(node_t *head, const char *str) {
    if (head == NULL || str == NULL) {
        return 0;
    }

    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (new_node == NULL) {
        perror("Unable to allocate memory for node");
        return -1;
    }

    int error_code = pthread_rwlock_init(&new_node->rwlock, NULL);
    if (error_code != 0) {
        print_error("Unable to init rwlock", error_code);
        free(new_node);
        return -1;
    }

    new_node->value = strdup(str);
    if (new_node->value == NULL) {
        perror("Unable to allocate memory for node value");
        free_node(new_node);
        return -1;
    }

    if (write_lock_node_rwlock(head) != 0) {
        free_node(new_node);
        return -1;
    }

    new_node->next = head->next;
    head->next = new_node;

    return unlock_node_rwlock(head) == 0 ? 0 : -1;
}

int list_sort(node_t *head) {
    if (head == NULL) {
        return -1;
    }

    node_t *prev, *cur, *next;
    int disordered = TRUE;

    while (disordered) {
        disordered = FALSE;
        prev = head;

        if (write_lock_node_rwlock(prev) != 0) return -1;

        cur = prev->next;
        if (cur != NULL) {
            if (write_lock_node_rwlock(cur) != 0) return -1;

            next = cur->next;
            while (next != NULL) {
                if (write_lock_node_rwlock(next) != 0) return -1;

                if (!STR_ORDERED(cur->value, next->value)) {
                    disordered = TRUE;
                    cur->next = next->next;
                    next->next = cur;
                    prev->next = next;

                    cur = next;
                    next = cur->next;
                }

                if (unlock_node_rwlock(prev) != 0) return -1;

                prev = cur;
                cur = next;
                next = next->next;

                sleep(MID_SORT_WAIT_SECONDS);
            }

            if (unlock_node_rwlock(cur) != 0) return -1;
        }

        if (unlock_node_rwlock(prev) != 0) return -1;
    }

    return 0;
}

int list_print(node_t *head) {
    if (head == NULL) {
        printf("--List head is NULL--\n");
        return 0;
    }

    node_t *prev = head;
    if (read_lock_node_rwlock(prev) != 0) return -1;

    printf("--Your list--\n");
    node_t *cur = prev->next;
    while (cur != NULL) {
        if (read_lock_node_rwlock(cur) != 0) return -1;
        if (unlock_node_rwlock(prev) != 0) return -1;

        printf("%s\n", cur->value);

        prev = cur;
        cur = cur->next;
    }
    printf("--End of list--\n");

    if (unlock_node_rwlock(prev) != 0) return -1;

    return 0;
}

void *interval_list_sorting(void *param) {
    int sleep_time = (int)param;
    if (param <= 0) {
        fprintf(stderr, "interval_list_sorting: sleep_time must be >= 0!\n");
        return NULL;
    }

    while (global_list_head != NULL) {
        sleep(sleep_time);

        if (list_sort(global_list_head) == -1) {
            fprintf(stderr, "Sorting thread encountered error. Sorting stopped.\n");
            break;
        }
    }

    return NULL;
}

int get_strings() {
    if (global_list_head == NULL) {
        fprintf(stderr, "get_strings: global_list_head is NULL\n");
        return -1;
    }

    char buf[BUF_SIZE + 1];
    while (1) {
        ssize_t bytes_read = read(STDIN_FILENO, buf, BUF_SIZE);
        if (bytes_read == -1) {
            perror("Unable to read from stdin");
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        buf[bytes_read] = '\0';
        if (buf[bytes_read - 1] == '\n') {
            buf[bytes_read - 1] = '\0';
            bytes_read--;
        }

        if (bytes_read == 0) {
            if (list_print(global_list_head) == -1) {
                return -1;
            }
            continue;
        }

        if (list_insert(global_list_head, buf) == -1) {
            return -1;
        }
    }

    return 0;
}

int main() {
    pthread_t threads[NUMBER_OF_SORTING_THREADS];
    int thread_sort_intervals_seconds[NUMBER_OF_SORTING_THREADS] = { 5 };
    int error_code;

    global_list_head = list_create();
    if (global_list_head == NULL) {
        return EXIT_FAILURE;
    }

    int num_of_created = 0;
    for (int i = 0; i < NUMBER_OF_SORTING_THREADS; i++) {
        error_code = pthread_create(&threads[i], NULL, interval_list_sorting, (void *)thread_sort_intervals_seconds[i]);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            break;
        }
        num_of_created++;
    }
    printf("Created %d of %d sorting threads\n", num_of_created, NUMBER_OF_SORTING_THREADS);

    get_strings();

    for (int i = 0; i < num_of_created; i++) {
        pthread_cancel(threads[i]);
    }

    for (int i = 0; i < num_of_created; i++) {
        pthread_join(threads[i], NULL);
    }

    list_destroy(global_list_head);
    global_list_head = NULL;

    pthread_exit(NULL);
}
