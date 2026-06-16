#include "thread_utils.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

int create_server_thread_with_stack(pthread_t *thread, size_t stack_size, void *(*start_routine)(void *), void *arg) {
  // Try the requested stack size first — some threads (chunk generator,
  // chunk streamer) need a large stack and will overflow with the default.
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);
  if (ret == 0) {
    #ifdef PTHREAD_STACK_MIN
    if (stack_size < (size_t)PTHREAD_STACK_MIN) stack_size = (size_t)PTHREAD_STACK_MIN;
    #endif

    // Round up to page size to avoid EINVAL on strict systems.
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
      size_t page_mask = (size_t)page_size - 1;
      if (stack_size & page_mask) {
        stack_size = (stack_size + page_mask) & ~page_mask;
      }
    }

    ret = pthread_attr_setstacksize(&attr, stack_size);
    if (ret == 0) {
      ret = pthread_create(thread, &attr, start_routine, arg);
    }
    int destroy_ret = pthread_attr_destroy(&attr);
    if (ret == 0) {
      if (destroy_ret != 0) return destroy_ret;
      return 0;
    }
  }

  // Explicit stack size failed — try with default stack attributes.
  // This can happen on systems with unusual page sizes or limits.
  return pthread_create(thread, NULL, start_routine, arg);
}

int create_server_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
  return create_server_thread_with_stack(thread, IRONGINGOT_THREAD_STACK_SIZE, start_routine, arg);
}
