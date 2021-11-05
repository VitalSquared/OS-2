#include <stdio.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#define TRUE 1
#define FALSE 0

#define NUM_DETAIL_TYPES 5

const char *detail_names[NUM_DETAIL_TYPES] = { "Detail A", "Detail B", "Detail C", "Module", "Widget" };
const int detail_create_times[NUM_DETAIL_TYPES] = { 1, 2, 3, 0, 0 };
const int *detail_dependencies[NUM_DETAIL_TYPES] =
        {
            (int[]) { -1 },
            (int[]) { -1 },
            (int[]) { -1 },
            (int[]) { 0, 1, -1 },
            (int[]) { 2, 3, -1 },
        };

sem_t semaphores[NUM_DETAIL_TYPES];
int should_create = TRUE;

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

void post_semaphore(int sem_num) {
    if (sem_post(&semaphores[sem_num]) == -1) {
        perror("Unable to post semaphore");
    }
}

void wait_semaphore(int sem_num) {
    if (sem_wait(&semaphores[sem_num]) == -1) {
        perror("Unable to wait semaphore");
    }
}

void *create_detail(void *param) {
    int detail_type = (int)param;

    while (should_create) {
        for (int i = 0; detail_dependencies[detail_type][i] != -1; i++) {
            wait_semaphore(detail_dependencies[detail_type][i]);
        }
        sleep(detail_create_times[detail_type]);
        post_semaphore(detail_type);
        printf("%s created!\n", detail_names[detail_type]);
    }

    return param;
}

int init_signal_mask(sigset_t *sigset, int sig) {
    if (sigemptyset(sigset) == -1 || sigaddset(sigset, sig) == -1 || sigprocmask(SIG_BLOCK, sigset, NULL) == -1) {
        perror("Unable to init process signal mask");
        return -1;
    }
    return 0;
}

int wait_for_signal(sigset_t *sigset, int sig) {
    int res;
    do {
        if (sigwait(sigset, &res) == -1) {
            perror("sigwait error");
            return -1;
        }
    } while (sig != res);
    fprintf(stderr, " signal caught. Stopping production\n");
    return 0;
}

void print_stats() {
    for (int i = 0; i < NUM_DETAIL_TYPES; i++) {
        int val = -1;
        sem_getvalue(&semaphores[i], &val);
        printf("%s: %d\n", detail_names[i], val);
    }
}

int main() {
    sigset_t sigset;
    if (init_signal_mask(&sigset, SIGINT) == -1) {
        return EXIT_FAILURE;
    }

    int sem_init_values[NUM_DETAIL_TYPES] = { 0, 0, 0, 0 };
    for (int i = 0; i < NUM_DETAIL_TYPES; i++) {
        if (sem_init(&semaphores[i], 0, sem_init_values[i]) == -1) {
            perror("Unable to init semaphore");
            for (int j = 0; j < i; j++) {
                sem_destroy(&semaphores[j]);
            }
            return EXIT_FAILURE;
        }
    }

    pthread_t threads[NUM_DETAIL_TYPES];

    int error_code;
    int num_of_created = 0;
    for (int i = 0; i < NUM_DETAIL_TYPES; i++) {
        error_code = pthread_create(&threads[i], NULL, create_detail, (void *)i);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            break;
        }
        num_of_created++;
    }

    if (num_of_created == NUM_DETAIL_TYPES) {
        wait_for_signal(&sigset, SIGINT);
    }

    should_create = FALSE;

    for (int i = 0; i < num_of_created; i++) {
        pthread_cancel(threads[i]);
    }
    for (int i = 0; i < num_of_created; i++) {
        pthread_join(threads[i], NULL);
    }

    if (num_of_created == NUM_DETAIL_TYPES) {
        print_stats();
    }

    for (int i = 0; i < num_of_created; i++) {
        sem_destroy(&semaphores[i]);
    }
    pthread_exit(NULL);
}
