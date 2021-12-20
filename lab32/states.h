#ifndef LAB32_STATES_H
#define LAB32_STATES_H

//#define DROP_HTTP_NO_CLIENTS

#define BUF_SIZE 4096

#define INTERNAL_ERROR 0
#define CONNECTION_ERROR 1
#define INVALID_REQUEST 2
#define NOT_A_GET_METHOD 3
#define HOST_NOT_FOUND_CUSTOM 4
#define TRY_AGAIN_CUSTOM 5
#define NO_RECOVERY_CUSTOM 6
#define NO_DATA_CUSTOM 7
#define UNKNOWN_ERROR 8

#define GETTING_FROM_CACHE 2    //only for client
#define DOWNLOADING 1
#define AWAITING_REQUEST 0
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)
#define NON_SOCK_ERROR (-3)

#define HTTP_NO_HEADERS (-1)

#define HTTP_CODE_UNDEFINED (-1)
#define HTTP_CODE_NONE 0

#define HTTP_CONTENT_LENGTH (1)
#define HTTP_CHUNKED (0)

#define TRUE 1
#define FALSE 0

#define INFO_LOG TRUE
#define ERROR_LOG TRUE

#define STR_EQ(STR1, STR2) (strcmp(STR1, STR2) == 0)
#define IS_PORT_VALID(PORT) (0 < (PORT) && (PORT) <= 0xFFFF)
#define MAX(A, B) ((A) > (B) ? (A) : (B))

void print_error(const char *prefix, int code);
int convert_number(char *str, int *number);
int strings_equal_by_length(const char *str1, size_t len1, const char *str2, size_t len2);
int get_number_from_string_by_length(const char *str, size_t length);
int read_lock_rwlock(pthread_rwlock_t *rwlock, const char *error);
int write_lock_rwlock(pthread_rwlock_t *rwlock, const char *error);
int unlock_rwlock(pthread_rwlock_t *rwlock, const char *error);

#endif
