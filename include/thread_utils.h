#ifndef H_THREAD_UTILS
#define H_THREAD_UTILS

#include <pthread.h>

// musl's default pthread stack is small and can vary by build. Keep most
// workers modest, but let chunk-heavy workers opt into a larger stack.
#define IRONGINGOT_THREAD_STACK_SIZE (128u * 1024u)
#define IRONGINGOT_CHUNK_THREAD_STACK_SIZE (256u * 1024u)
#define IRONGINGOT_POOL_THREAD_STACK_SIZE (128u * 1024u)

int create_server_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg);
int create_server_thread_with_stack(pthread_t *thread, size_t stack_size, void *(*start_routine)(void *), void *arg);

#endif
