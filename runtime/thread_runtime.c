#include <stdint.h>
#include <stdlib.h>

#if defined(__has_include)
#if __has_include(<threads.h>)
#include <threads.h>
#define CYAN_HAVE_C11_THREADS 1
#elif __has_include(<pthread.h>)
#include <pthread.h>
#define CYAN_HAVE_PTHREADS 1
#else
#error "no thread runtime backend available"
#endif
#else
#include <threads.h>
#define CYAN_HAVE_C11_THREADS 1
#endif

typedef void (*cyan_run_task_fn)(void *);

typedef struct {
    void *data;
    const void *vtable;
} cyan_iface_value;

typedef struct {
    const cyan_iface_value *task;
} cyan_thread_job;

static int cyan_thread_entry(void *arg);

#if defined(CYAN_HAVE_C11_THREADS)
typedef thrd_t cyan_thread_handle;

static int cyan_thread_create(cyan_thread_handle *thread,
                              cyan_thread_job *job) {
    return thrd_create(thread, cyan_thread_entry, job) == thrd_success;
}

static void cyan_thread_join(cyan_thread_handle thread) {
    int result = 0;
    thrd_join(thread, &result);
}
#elif defined(CYAN_HAVE_PTHREADS)
typedef pthread_t cyan_thread_handle;

static void *cyan_pthread_entry(void *arg) {
    cyan_thread_entry(arg);
    return NULL;
}

static int cyan_thread_create(cyan_thread_handle *thread,
                              cyan_thread_job *job) {
    return pthread_create(thread, NULL, cyan_pthread_entry, job) == 0;
}

static void cyan_thread_join(cyan_thread_handle thread) {
    pthread_join(thread, NULL);
}
#endif

static void cyan_run_one(const cyan_iface_value *task) {
    if (task == NULL || task->vtable == NULL) {
        return;
    }
    const void *const *vtable = (const void *const *)task->vtable;
    if (vtable[0] == NULL) {
        return;
    }
    ((cyan_run_task_fn)vtable[0])(task->data);
}

static int cyan_thread_entry(void *arg) {
    const cyan_thread_job *job = (const cyan_thread_job *)arg;
    if (job != NULL) {
        cyan_run_one(job->task);
    }
    return 0;
}

int cyan_runtime_parallel_do(const void *tasks_raw, int64_t count) {
    if (count <= 0 || tasks_raw == NULL) {
        return 0;
    }

    const cyan_iface_value *tasks = (const cyan_iface_value *)tasks_raw;
    if (count == 1) {
        cyan_run_one(&tasks[0]);
        return 0;
    }

    cyan_thread_handle *threads = (cyan_thread_handle *)malloc(
        (size_t)(count - 1) * sizeof(cyan_thread_handle));
    cyan_thread_job *jobs = (cyan_thread_job *)malloc((size_t)(count - 1) *
                                                      sizeof(cyan_thread_job));
    if (threads == NULL || jobs == NULL) {
        free(jobs);
        free(threads);
        for (int64_t index = 0; index < count; ++index) {
            cyan_run_one(&tasks[index]);
        }
        return 0;
    }

    int64_t started = 0;
    for (int64_t index = 1; index < count; ++index) {
        jobs[started].task = &tasks[index];
        if (!cyan_thread_create(&threads[started], &jobs[started])) {
            cyan_run_one(&tasks[index]);
            continue;
        }
        ++started;
    }

    cyan_run_one(&tasks[0]);

    for (int64_t index = 0; index < started; ++index) {
        cyan_thread_join(threads[index]);
    }

    free(jobs);
    free(threads);
    return 0;
}
