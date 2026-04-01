#ifndef H_THREAD_POOL
#define H_THREAD_POOL

#include <pthread.h>
#include <stdint.h>

// Thread pool task function type
typedef void (*task_func_t)(void* arg);

// Task queue node
typedef struct Task {
  task_func_t func;
  void* arg;
  struct Task* next;
} Task;

// Thread pool structure
typedef struct {
  pthread_t* threads;       // Array of worker threads
  Task* task_queue_head;    // Head of task queue
  Task* task_queue_tail;    // Tail of task queue
  pthread_mutex_t mutex;    // Mutex for task queue access
  pthread_cond_t cond;      // Condition variable for task notification
  int num_threads;          // Number of worker threads
  int shutdown;             // Shutdown flag
  int active_tasks;         // Number of currently executing tasks
  pthread_cond_t done_cond; // Condition variable for completion waiting
} ThreadPool;

// Initialize thread pool with specified number of threads
int thread_pool_init(ThreadPool* pool, int num_threads);

// Submit a task to the thread pool
int thread_pool_submit(ThreadPool* pool, task_func_t func, void* arg);

// Wait for all tasks to complete
void thread_pool_wait(ThreadPool* pool);

// Shutdown and destroy the thread pool
void thread_pool_destroy(ThreadPool* pool);

// Get number of available CPU cores
int get_cpu_count(void);

#endif
