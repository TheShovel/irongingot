#include "thread_utils.h"

#include <errno.h>
#include <stddef.h>

int create_server_thread_with_stack(pthread_t *thread, size_t stack_size, void *(*start_routine)(void *), void *arg) {
  pthread_attr_t attr;
  int ret = pthread_attr_init(&attr);
  if (ret != 0) return ret;

  #ifdef PTHREAD_STACK_MIN
  if (stack_size < (size_t)PTHREAD_STACK_MIN) stack_size = (size_t)PTHREAD_STACK_MIN;
  #endif

  ret = pthread_attr_setstacksize(&attr, stack_size);
  if (ret == 0) {
    ret = pthread_create(thread, &attr, start_routine, arg);
  }

  int destroy_ret = pthread_attr_destroy(&attr);
  if (ret == 0 && destroy_ret != 0) return destroy_ret;
  return ret;
}

int create_server_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg) {
  return create_server_thread_with_stack(thread, IRONGINGOT_THREAD_STACK_SIZE, start_routine, arg);
}
