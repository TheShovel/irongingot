#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/sysinfo.h>
#endif

#include "thread_pool.h"

// Worker thread function
static void* worker_thread_func(void* arg) {
  ThreadPool* pool = (ThreadPool*)arg;

  while (1) {
    pthread_mutex_lock(&pool->mutex);

    // Wait for task or shutdown
    while (pool->task_queue_head == NULL && !pool->shutdown) {
      pthread_cond_wait(&pool->cond, &pool->mutex);
    }

    // Check for shutdown
    if (pool->shutdown && pool->task_queue_head == NULL) {
      pthread_mutex_unlock(&pool->mutex);
      break;
    }

    // Dequeue task
    Task* task = pool->task_queue_head;
    if (task != NULL) {
      pool->task_queue_head = task->next;
      if (pool->task_queue_head == NULL) {
        pool->task_queue_tail = NULL;
      }
      pool->active_tasks++;
    }

    pthread_mutex_unlock(&pool->mutex);

    // Execute task
    if (task != NULL) {
      task->func(task->arg);
      free(task);

      pthread_mutex_lock(&pool->mutex);
      pool->active_tasks--;
      if (pool->active_tasks == 0) {
        pthread_cond_signal(&pool->done_cond);
      }
      pthread_mutex_unlock(&pool->mutex);
    }
  }

  return NULL;
}

int thread_pool_init(ThreadPool* pool, int num_threads) {
  if (pool == NULL || num_threads <= 0) {
    return -1;
  }

  memset(pool, 0, sizeof(ThreadPool));
  pool->num_threads = num_threads;
  pool->shutdown = 0;
  pool->active_tasks = 0;

  // Allocate thread array
  pool->threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
  if (pool->threads == NULL) {
    return -1;
  }

  // Initialize synchronization primitives
  if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
    free(pool->threads);
    return -1;
  }

  if (pthread_cond_init(&pool->cond, NULL) != 0) {
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    return -1;
  }

  if (pthread_cond_init(&pool->done_cond, NULL) != 0) {
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    return -1;
  }

  // Create worker threads
  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, worker_thread_func, pool) != 0) {
      // Cleanup on failure
      pool->shutdown = 1;
      pthread_cond_broadcast(&pool->cond);
      for (int j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }
      pthread_cond_destroy(&pool->done_cond);
      pthread_cond_destroy(&pool->cond);
      pthread_mutex_destroy(&pool->mutex);
      free(pool->threads);
      return -1;
    }
  }

  return 0;
}

int thread_pool_submit(ThreadPool* pool, task_func_t func, void* arg) {
  if (pool == NULL || func == NULL) {
    return -1;
  }

  // Create task
  Task* task = (Task*)malloc(sizeof(Task));
  if (task == NULL) {
    return -1;
  }

  task->func = func;
  task->arg = arg;
  task->next = NULL;

  pthread_mutex_lock(&pool->mutex);

  // Check for shutdown
  if (pool->shutdown) {
    pthread_mutex_unlock(&pool->mutex);
    free(task);
    return -1;
  }

  // Enqueue task
  if (pool->task_queue_tail == NULL) {
    pool->task_queue_head = task;
    pool->task_queue_tail = task;
  } else {
    pool->task_queue_tail->next = task;
    pool->task_queue_tail = task;
  }

  pthread_cond_signal(&pool->cond);
  pthread_mutex_unlock(&pool->mutex);

  return 0;
}

void thread_pool_wait(ThreadPool* pool) {
  if (pool == NULL) {
    return;
  }

  pthread_mutex_lock(&pool->mutex);
  while (pool->active_tasks > 0 || pool->task_queue_head != NULL) {
    pthread_cond_wait(&pool->done_cond, &pool->mutex);
  }
  pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_destroy(ThreadPool* pool) {
  if (pool == NULL) {
    return;
  }

  pthread_mutex_lock(&pool->mutex);
  pool->shutdown = 1;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->mutex);

  // Join all threads
  for (int i = 0; i < pool->num_threads; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  // Free remaining tasks
  Task* task = pool->task_queue_head;
  while (task != NULL) {
    Task* next = task->next;
    free(task);
    task = next;
  }

  pthread_cond_destroy(&pool->done_cond);
  pthread_cond_destroy(&pool->cond);
  pthread_mutex_destroy(&pool->mutex);
  free(pool->threads);
}

int get_cpu_count(void) {
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return (int)sysinfo.dwNumberOfProcessors;
#else
  return get_nprocs();
#endif
}
