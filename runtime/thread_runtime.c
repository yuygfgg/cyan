#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

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

#if defined(CYAN_HAVE_PTHREADS) && defined(__has_include)
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
#endif

#if defined(CYAN_HAVE_C11_THREADS)
typedef mtx_t cyan_mutex_t;
typedef cnd_t cyan_cond_t;
typedef thrd_t cyan_thread_t;
typedef once_flag cyan_once_t;
#define CYAN_ONCE_INIT ONCE_FLAG_INIT

static bool cyan_mutex_init(cyan_mutex_t *mutex) {
    return mtx_init(mutex, mtx_plain) == thrd_success;
}

static void cyan_mutex_destroy(cyan_mutex_t *mutex) { mtx_destroy(mutex); }

static void cyan_mutex_lock(cyan_mutex_t *mutex) { (void)mtx_lock(mutex); }

static void cyan_mutex_unlock(cyan_mutex_t *mutex) { (void)mtx_unlock(mutex); }

static bool cyan_cond_init(cyan_cond_t *cond) {
    return cnd_init(cond) == thrd_success;
}

static void cyan_cond_destroy(cyan_cond_t *cond) { cnd_destroy(cond); }

static void cyan_cond_signal(cyan_cond_t *cond) { (void)cnd_signal(cond); }

static void cyan_cond_broadcast(cyan_cond_t *cond) {
    (void)cnd_broadcast(cond);
}

static void cyan_cond_wait(cyan_cond_t *cond, cyan_mutex_t *mutex) {
    (void)cnd_wait(cond, mutex);
}

static void cyan_cond_timedwait(cyan_cond_t *cond, cyan_mutex_t *mutex,
                                const struct timespec *deadline) {
    (void)cnd_timedwait(cond, mutex, deadline);
}

static bool cyan_thread_create(cyan_thread_t *thread, int (*entry)(void *),
                               void *arg) {
    return thrd_create(thread, entry, arg) == thrd_success;
}

static void cyan_call_once(cyan_once_t *once, void (*entry)(void)) {
    call_once(once, entry);
}
#else
typedef pthread_mutex_t cyan_mutex_t;
typedef pthread_cond_t cyan_cond_t;
typedef pthread_t cyan_thread_t;
typedef pthread_once_t cyan_once_t;
#define CYAN_ONCE_INIT PTHREAD_ONCE_INIT

static bool cyan_mutex_init(cyan_mutex_t *mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}

static void cyan_mutex_destroy(cyan_mutex_t *mutex) {
    pthread_mutex_destroy(mutex);
}

static void cyan_mutex_lock(cyan_mutex_t *mutex) { pthread_mutex_lock(mutex); }

static void cyan_mutex_unlock(cyan_mutex_t *mutex) {
    pthread_mutex_unlock(mutex);
}

static bool cyan_cond_init(cyan_cond_t *cond) {
    return pthread_cond_init(cond, NULL) == 0;
}

static void cyan_cond_destroy(cyan_cond_t *cond) { pthread_cond_destroy(cond); }

static void cyan_cond_signal(cyan_cond_t *cond) { pthread_cond_signal(cond); }

static void cyan_cond_broadcast(cyan_cond_t *cond) {
    pthread_cond_broadcast(cond);
}

static void cyan_cond_wait(cyan_cond_t *cond, cyan_mutex_t *mutex) {
    pthread_cond_wait(cond, mutex);
}

static void cyan_cond_timedwait(cyan_cond_t *cond, cyan_mutex_t *mutex,
                                const struct timespec *deadline) {
    pthread_cond_timedwait(cond, mutex, deadline);
}

static bool cyan_thread_create(cyan_thread_t *thread, void *(*entry)(void *),
                               void *arg) {
    return pthread_create(thread, NULL, entry, arg) == 0;
}

static void cyan_call_once(cyan_once_t *once, void (*entry)(void)) {
    pthread_once(once, entry);
}
#endif

typedef void (*cyan_run_task_fn)(void *);
typedef void (*cyan_runtime_thunk_fn)(void *);

typedef struct {
    void *data;
    const void *vtable;
} cyan_iface_value;

typedef struct cyan_wait_group {
    cyan_mutex_t mutex;
    cyan_cond_t cond;
    int64_t pending;
} cyan_wait_group;

typedef struct cyan_join_state {
    cyan_mutex_t mutex;
    cyan_cond_t cond;
    int refcount;
    bool completed;
} cyan_join_state;

typedef struct cyan_task {
    struct cyan_task *next;
    void *payload;
    cyan_runtime_thunk_fn run;
    cyan_runtime_thunk_fn drop;
    cyan_wait_group *group;
    cyan_join_state *join_state;
} cyan_task;

typedef struct {
    cyan_mutex_t mutex;
    cyan_cond_t cond;
    cyan_task *head;
    cyan_task *tail;
    cyan_thread_t *workers;
    size_t worker_count;
    bool sync_initialized;
    bool ready;
} cyan_executor_state;

static cyan_executor_state cyan_global_executor = {
    .head = NULL,
    .tail = NULL,
    .workers = NULL,
    .worker_count = 0,
    .sync_initialized = false,
    .ready = false,
};
static cyan_once_t cyan_executor_once = CYAN_ONCE_INIT;

static void cyan_run_one(void *payload);
static void cyan_execute_task(cyan_task *task);
static bool cyan_try_pop_task(cyan_task **out_task);
static bool cyan_help_execute_one(void);
static void cyan_cond_timedwait_ms(cyan_cond_t *cond, cyan_mutex_t *mutex,
                                   long ms);
static void cyan_executor_init_once(void);

#if defined(CYAN_HAVE_C11_THREADS)
static int cyan_worker_main(void *unused);
#else
static void *cyan_worker_main(void *unused);
#endif

static bool cyan_wait_group_init(cyan_wait_group *group) {
    if (!cyan_mutex_init(&group->mutex)) {
        return false;
    }
    if (!cyan_cond_init(&group->cond)) {
        cyan_mutex_destroy(&group->mutex);
        return false;
    }
    group->pending = 0;
    return true;
}

static void cyan_wait_group_destroy(cyan_wait_group *group) {
    cyan_cond_destroy(&group->cond);
    cyan_mutex_destroy(&group->mutex);
}

static void cyan_wait_group_add(cyan_wait_group *group) {
    cyan_mutex_lock(&group->mutex);
    ++group->pending;
    cyan_mutex_unlock(&group->mutex);
}

static void cyan_wait_group_task_done(cyan_wait_group *group) {
    cyan_mutex_lock(&group->mutex);
    if (group->pending > 0) {
        --group->pending;
    }
    if (group->pending == 0) {
        cyan_cond_broadcast(&group->cond);
    }
    cyan_mutex_unlock(&group->mutex);
}

static void cyan_wait_group_wait(cyan_wait_group *group) {
    for (;;) {
        cyan_mutex_lock(&group->mutex);
        const bool done = group->pending == 0;
        cyan_mutex_unlock(&group->mutex);
        if (done) {
            return;
        }
        if (cyan_help_execute_one()) {
            continue;
        }

        cyan_mutex_lock(&group->mutex);
        if (group->pending != 0) {
            cyan_cond_timedwait_ms(&group->cond, &group->mutex, 1);
        }
        cyan_mutex_unlock(&group->mutex);
    }
}

static cyan_join_state *cyan_join_state_create(void) {
    cyan_join_state *state = (cyan_join_state *)malloc(sizeof(cyan_join_state));
    if (state == NULL) {
        return NULL;
    }
    if (!cyan_mutex_init(&state->mutex)) {
        free(state);
        return NULL;
    }
    if (!cyan_cond_init(&state->cond)) {
        cyan_mutex_destroy(&state->mutex);
        free(state);
        return NULL;
    }
    state->refcount = 1;
    state->completed = false;
    return state;
}

static void cyan_join_state_retain(cyan_join_state *state) {
    if (state == NULL) {
        return;
    }
    cyan_mutex_lock(&state->mutex);
    ++state->refcount;
    cyan_mutex_unlock(&state->mutex);
}

static void cyan_join_state_complete(cyan_join_state *state) {
    if (state == NULL) {
        return;
    }
    cyan_mutex_lock(&state->mutex);
    state->completed = true;
    cyan_cond_broadcast(&state->cond);
    cyan_mutex_unlock(&state->mutex);
}

static void cyan_join_state_wait(cyan_join_state *state) {
    if (state == NULL) {
        return;
    }
    for (;;) {
        cyan_mutex_lock(&state->mutex);
        const bool done = state->completed;
        cyan_mutex_unlock(&state->mutex);
        if (done) {
            return;
        }
        if (cyan_help_execute_one()) {
            continue;
        }

        cyan_mutex_lock(&state->mutex);
        if (!state->completed) {
            cyan_cond_timedwait_ms(&state->cond, &state->mutex, 1);
        }
        cyan_mutex_unlock(&state->mutex);
    }
}

static void cyan_join_state_release(cyan_join_state *state) {
    if (state == NULL) {
        return;
    }

    bool should_destroy = false;
    cyan_mutex_lock(&state->mutex);
    --state->refcount;
    should_destroy = state->refcount == 0;
    cyan_mutex_unlock(&state->mutex);

    if (should_destroy) {
        cyan_cond_destroy(&state->cond);
        cyan_mutex_destroy(&state->mutex);
        free(state);
    }
}

static bool cyan_deadline_after_ms(struct timespec *deadline, long ms) {
#if defined(CYAN_HAVE_C11_THREADS)
    if (timespec_get(deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }
#else
#if defined(CLOCK_REALTIME)
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return false;
    }
#else
    if (timespec_get(deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }
#endif
#endif
    deadline->tv_nsec += ms * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += deadline->tv_nsec / 1000000000L;
        deadline->tv_nsec %= 1000000000L;
    }
    return true;
}

static void cyan_cond_timedwait_ms(cyan_cond_t *cond, cyan_mutex_t *mutex,
                                   long ms) {
    struct timespec deadline;
    if (!cyan_deadline_after_ms(&deadline, ms)) {
        cyan_cond_wait(cond, mutex);
        return;
    }
    cyan_cond_timedwait(cond, mutex, &deadline);
}

static void cyan_run_one(void *payload) {
    const cyan_iface_value *task = (const cyan_iface_value *)payload;
    if (task == NULL || task->vtable == NULL) {
        return;
    }
    const void *const *vtable = (const void *const *)task->vtable;
    if (vtable[0] == NULL) {
        return;
    }
    ((cyan_run_task_fn)vtable[0])(task->data);
}

static bool cyan_enqueue_task(cyan_task *task) {
    if (!cyan_global_executor.ready || task == NULL) {
        return false;
    }

    cyan_mutex_lock(&cyan_global_executor.mutex);
    task->next = NULL;
    if (cyan_global_executor.tail != NULL) {
        cyan_global_executor.tail->next = task;
    } else {
        cyan_global_executor.head = task;
    }
    cyan_global_executor.tail = task;
    cyan_cond_signal(&cyan_global_executor.cond);
    cyan_mutex_unlock(&cyan_global_executor.mutex);
    return true;
}

static bool cyan_try_pop_task(cyan_task **out_task) {
    if (out_task == NULL) {
        return false;
    }
    if (!cyan_global_executor.sync_initialized) {
        *out_task = NULL;
        return false;
    }

    cyan_mutex_lock(&cyan_global_executor.mutex);
    cyan_task *task = cyan_global_executor.head;
    if (task != NULL) {
        cyan_global_executor.head = task->next;
        if (cyan_global_executor.head == NULL) {
            cyan_global_executor.tail = NULL;
        }
    }
    cyan_mutex_unlock(&cyan_global_executor.mutex);

    *out_task = task;
    return task != NULL;
}

static void cyan_execute_task(cyan_task *task) {
    if (task == NULL) {
        return;
    }

    if (task->run != NULL) {
        task->run(task->payload);
    }
    if (task->drop != NULL) {
        task->drop(task->payload);
    }
    if (task->group != NULL) {
        cyan_wait_group_task_done(task->group);
    }
    if (task->join_state != NULL) {
        cyan_join_state_complete(task->join_state);
        cyan_join_state_release(task->join_state);
    }
    free(task);
}

static bool cyan_help_execute_one(void) {
    cyan_task *task = NULL;
    if (!cyan_try_pop_task(&task)) {
        return false;
    }
    cyan_execute_task(task);
    return true;
}

#if defined(CYAN_HAVE_C11_THREADS)
static int cyan_worker_main(void *unused) {
#else
static void *cyan_worker_main(void *unused) {
#endif
    (void)unused;
    for (;;) {
        cyan_mutex_lock(&cyan_global_executor.mutex);
        while (cyan_global_executor.head == NULL) {
            cyan_cond_wait(&cyan_global_executor.cond,
                           &cyan_global_executor.mutex);
        }
        cyan_task *task = cyan_global_executor.head;
        cyan_global_executor.head = task->next;
        if (cyan_global_executor.head == NULL) {
            cyan_global_executor.tail = NULL;
        }
        cyan_mutex_unlock(&cyan_global_executor.mutex);

        cyan_execute_task(task);
    }
#if defined(CYAN_HAVE_C11_THREADS)
    return 0;
#else
    return NULL;
#endif
}

static long cyan_detect_cpu_count(void) {
#if defined(CYAN_HAVE_PTHREADS) && defined(_SC_NPROCESSORS_ONLN)
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0) {
        return cpu_count;
    }
#endif
    return 4;
}

static void cyan_executor_init_once(void) {
    if (!cyan_mutex_init(&cyan_global_executor.mutex)) {
        return;
    }
    if (!cyan_cond_init(&cyan_global_executor.cond)) {
        cyan_mutex_destroy(&cyan_global_executor.mutex);
        return;
    }
    cyan_global_executor.sync_initialized = true;

    long cpu_count = cyan_detect_cpu_count();
    if (cpu_count < 1) {
        cpu_count = 4;
    }

    size_t worker_count = (size_t)cpu_count;
    cyan_thread_t *workers =
        (cyan_thread_t *)malloc(worker_count * sizeof(cyan_thread_t));
    if (workers == NULL) {
        return;
    }

    size_t created = 0;
    for (; created < worker_count; ++created) {
        if (!cyan_thread_create(&workers[created], cyan_worker_main, NULL)) {
            break;
        }
    }
    if (created == 0) {
        free(workers);
        return;
    }

    cyan_global_executor.workers = workers;
    cyan_global_executor.worker_count = created;
    cyan_global_executor.ready = true;
}

static bool cyan_executor_ensure(void) {
    cyan_call_once(&cyan_executor_once, cyan_executor_init_once);
    return cyan_global_executor.ready;
}

int cyan_runtime_parallel_do(const void *tasks_raw, int64_t count) {
    if (count <= 0 || tasks_raw == NULL) {
        return 0;
    }

    const cyan_iface_value *tasks = (const cyan_iface_value *)tasks_raw;
    if (count == 1) {
        cyan_run_one((void *)&tasks[0]);
        return 0;
    }

    if (!cyan_executor_ensure()) {
        for (int64_t index = 0; index < count; ++index) {
            cyan_run_one((void *)&tasks[index]);
        }
        return 0;
    }

    cyan_wait_group group;
    if (!cyan_wait_group_init(&group)) {
        for (int64_t index = 0; index < count; ++index) {
            cyan_run_one((void *)&tasks[index]);
        }
        return 0;
    }

    for (int64_t index = 1; index < count; ++index) {
        cyan_task *task = (cyan_task *)malloc(sizeof(cyan_task));
        if (task == NULL) {
            cyan_run_one((void *)&tasks[index]);
            continue;
        }
        task->next = NULL;
        task->payload = (void *)&tasks[index];
        task->run = cyan_run_one;
        task->drop = NULL;
        task->group = &group;
        task->join_state = NULL;
        cyan_wait_group_add(&group);
        if (!cyan_enqueue_task(task)) {
            cyan_wait_group_task_done(&group);
            free(task);
            cyan_run_one((void *)&tasks[index]);
        }
    }

    cyan_run_one((void *)&tasks[0]);
    cyan_wait_group_wait(&group);
    cyan_wait_group_destroy(&group);
    return 0;
}

void *cyan_runtime_spawn(void *payload, void *run_fn, void *drop_fn) {
    if (payload == NULL || run_fn == NULL || drop_fn == NULL) {
        return NULL;
    }
    if (!cyan_executor_ensure()) {
        return NULL;
    }

    cyan_join_state *state = cyan_join_state_create();
    if (state == NULL) {
        return NULL;
    }

    cyan_task *task = (cyan_task *)malloc(sizeof(cyan_task));
    if (task == NULL) {
        cyan_join_state_release(state);
        return NULL;
    }

    cyan_join_state_retain(state);
    task->next = NULL;
    task->payload = payload;
    task->run = (cyan_runtime_thunk_fn)run_fn;
    task->drop = (cyan_runtime_thunk_fn)drop_fn;
    task->group = NULL;
    task->join_state = state;

    if (!cyan_enqueue_task(task)) {
        cyan_join_state_release(state);
        cyan_join_state_release(state);
        free(task);
        return NULL;
    }

    return state;
}

void cyan_runtime_join(void *handle) {
    cyan_join_state *state = (cyan_join_state *)handle;
    if (state == NULL) {
        return;
    }
    cyan_join_state_wait(state);
    cyan_join_state_release(state);
}

void cyan_runtime_detach(void *handle) {
    cyan_join_state *state = (cyan_join_state *)handle;
    if (state == NULL) {
        return;
    }
    cyan_join_state_release(state);
}
