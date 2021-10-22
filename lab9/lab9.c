#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <math.h>

#define BLOCK_SIZE 2000

#define TRUE 1
#define FALSE 0

union sum_data {
    long long index;
    double partial_sum;
};

long number_of_threads = 0;
int should_stop = FALSE;
long long max_iterations = 0;
pthread_mutex_t max_iter_mutex = PTHREAD_MUTEX_INITIALIZER;

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

int lock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_lock(mutex);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
    }
    return error_code;
}

int unlock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_unlock(mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
    }
    return error_code;
}

void *calculate_partial_sum(void *param) {
    union sum_data *data = (union sum_data *)param;
    if (data == NULL) {
        fprintf(stderr, "calculate_partial_sum: invalid param\n");
        return data;
    }

    long long index = data->index;
    double partial_sum = 0.0;
    long iteration = 0;

    while (1) {
        if (should_stop && iteration != 0) {
            lock_mutex(&max_iter_mutex);
            if (iteration >= max_iterations) {
                unlock_mutex(&max_iter_mutex);
                break;
            }
            unlock_mutex(&max_iter_mutex);
        }
        else {
            lock_mutex(&max_iter_mutex);
            if (iteration > max_iterations) {
                max_iterations = iteration;
            }
            unlock_mutex(&max_iter_mutex);
            sched_yield();
        }

        long start = (iteration * number_of_threads + index) * BLOCK_SIZE;
        long end = start + BLOCK_SIZE;
        for (long long i = start; i < end; i++) {
            partial_sum += 1.0 / (i * 4.0 + 1.0);
            partial_sum -= 1.0 / (i * 4.0 + 3.0);
        }
        iteration++;
    }

    data->partial_sum = partial_sum;
    printf("Thread %lld finished, iterations = %ld, partial sum %.16f\n", index, iteration, data->partial_sum);
    return data;
}

long get_number_of_threads(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: %s threads_num\n", argv[0]);
        return 0;
    }

    errno = 0;  // errno is unique to calling thread (furthermore, we call this function before spawning any threads)
    char *endptr = "";
    long num = strtol(argv[1], &endptr, 10);

    if (errno != 0) {
        perror("Can't convert given number");
        return 0;
    }
    if (strcmp(endptr, "") != 0) {
        fprintf(stderr, "Number contains invalid symbols\n");
        return 0;
    }
    if (num < 1) {
        fprintf(stderr, "Invalid number of threads\n");
        return 0;
    }

    return num;
}

long create_threads(pthread_t *threads, union sum_data *data) {
    int error_code;
    long num_of_created = 0;

    for (long i = 0; i < number_of_threads && !should_stop; i++) {
        data[i].index = i;

        error_code = pthread_create(&threads[i], NULL, calculate_partial_sum, &data[i]);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            break;
        }

        num_of_created++;
    }

    return num_of_created;
}

int join_threads(pthread_t *threads, double *pi) {
    int error_code;
    int return_value = 0;
    double sum = 0.0;

    for (long i = 0; i < number_of_threads; i++) {
        union sum_data *res = NULL;

        error_code = pthread_join(threads[i], (void **)&res);
        if (error_code != 0) {
            print_error("Unable to join thread", error_code);
            return_value = -1;
            continue;
        }

        if (res == NULL) {
            fprintf(stderr, "Thread returned NULL\n");
            return_value = -1;
            continue;
        }

        sum += res->partial_sum;
    }
    (*pi) = sum * 4;

    return return_value;
}

void handle_sigint(int sig) {
    if (sig == SIGINT) {
        should_stop = TRUE;
    }
}

int main(int argc, char **argv) {
    number_of_threads = get_number_of_threads(argc, argv);
    if (number_of_threads == 0) {
        return EXIT_FAILURE;
    }

    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        return EXIT_FAILURE;
    }

    pthread_t threads[number_of_threads];
    union sum_data data[number_of_threads];
	
    long num_of_created = create_threads(threads, data);
    if (num_of_created < number_of_threads) {
        should_stop = TRUE;
        fprintf(stderr, "Couldn't create requested amount of threads!\n");
    }

    if (num_of_created == 0) {
        return EXIT_FAILURE;
    }

    double pi = 0.0;
    join_threads(threads, &pi);

    printf("pi = %.16f\n", pi);
    printf("real pi = %.16f\n", M_PI);
    return EXIT_SUCCESS;
}
