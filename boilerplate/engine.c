#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 16384
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define CONTROL_BACKLOG 16
#define SHUTDOWN_GRACE_TICKS 30
#define WAIT_TICK_USEC 100000

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

struct supervisor_ctx;

typedef struct {
    struct supervisor_ctx *ctx;
    char container_id[CONTAINER_ID_LEN];
    int read_fd;
} producer_ctx_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int exited;
    int stop_requested;
    char reason[32];
    char log_path[PATH_MAX];
    pthread_t producer_thread;
    int producer_started;
    int producer_joined;
    producer_ctx_t *producer_ctx;
    void *child_stack;
    pthread_cond_t state_changed;
    struct container_record *next;
} container_record_t;

typedef struct run_wait_ctx {
    struct supervisor_ctx *ctx;
    container_record_t *container;
    int client_fd;
} run_wait_ctx_t;

typedef struct supervisor_ctx {
    int server_fd;
    int monitor_fd;
    int signal_pipe[2];
    int should_stop;
    char base_rootfs[PATH_MAX];
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static int g_signal_pipe[2] = { -1, -1 };
static volatile sig_atomic_t g_supervisor_sigchld = 0;
static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_client_forward_stop = 0;
static char g_client_run_id[CONTAINER_ID_LEN];

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int ensure_directory(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static void build_log_path(const char *container_id, char *path, size_t path_len)
{
    snprintf(path, path_len, "%s/%s.log", LOG_DIR, container_id);
}

static int write_fully(int fd, const void *buffer, size_t len)
{
    const char *cursor = buffer;

    while (len > 0) {
        ssize_t written = write(fd, cursor, len);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        cursor += written;
        len -= (size_t)written;
    }

    return 0;
}

static int send_all(int fd, const void *buffer, size_t len)
{
    return write_fully(fd, buffer, len);
}

static int recv_all(int fd, void *buffer, size_t len)
{
    char *cursor = buffer;

    while (len > 0) {
        ssize_t rd = recv(fd, cursor, len, 0);

        if (rd == 0)
            return -1;
        if (rd < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        cursor += rd;
        len -= (size_t)rd;
    }

    return 0;
}

static void response_set(control_response_t *resp,
                         int status,
                         int exit_status,
                         const char *fmt,
                         ...)
{
    va_list ap;

    resp->status = status;
    resp->exit_status = exit_status;

    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static int append_format(char *buffer, size_t buffer_len, size_t *used, const char *fmt, ...)
{
    int written;
    va_list ap;

    if (*used >= buffer_len)
        return -1;

    va_start(ap, fmt);
    written = vsnprintf(buffer + *used, buffer_len - *used, fmt, ap);
    va_end(ap);

    if (written < 0)
        return -1;

    if ((size_t)written >= buffer_len - *used) {
        *used = buffer_len - 1U;
        return -1;
    }

    *used += (size_t)written;
    return 0;
}

static int is_live_state(container_state_t state)
{
    return state == CONTAINER_STARTING || state == CONTAINER_RUNNING;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cursor;

    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        if (strncmp(cursor->id, id, sizeof(cursor->id)) == 0)
            return cursor;
    }

    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cursor;

    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        if (cursor->host_pid == pid)
            return cursor;
    }

    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cursor;

    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        if (is_live_state(cursor->state) &&
            strncmp(cursor->rootfs, rootfs, sizeof(cursor->rootfs)) == 0)
            return 1;
    }

    return 0;
}

static void format_start_time(time_t started_at, char *buffer, size_t buffer_len)
{
    struct tm tm_value;

    if (started_at == 0) {
        snprintf(buffer, buffer_len, "-");
        return;
    }

    localtime_r(&started_at, &tm_value);
    strftime(buffer, buffer_len, "%Y-%m-%d %H:%M:%S", &tm_value);
}

static void supervisor_signal_handler(int signo)
{
    unsigned char byte = (unsigned char)signo;

    if (signo == SIGCHLD)
        g_supervisor_sigchld = 1;
    if (signo == SIGINT || signo == SIGTERM)
        g_supervisor_stop = 1;

    if (g_signal_pipe[1] >= 0)
        (void)write(g_signal_pipe[1], &byte, 1);
}

static int install_supervisor_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = supervisor_signal_handler;

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    return 0;
}

static void client_signal_handler(int signo)
{
    (void)signo;
    g_client_forward_stop = 1;
}

static int install_client_wait_handlers(struct sigaction *old_int, struct sigaction *old_term)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = client_signal_handler;

    if (sigaction(SIGINT, &sa, old_int) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, old_term) < 0) {
        sigaction(SIGINT, old_int, NULL);
        return -1;
    }

    return 0;
}

static void restore_client_wait_handlers(const struct sigaction *old_int,
                                         const struct sigaction *old_term)
{
    sigaction(SIGINT, old_int, NULL);
    sigaction(SIGTERM, old_term, NULL);
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (1) {
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        char path[PATH_MAX];
        int fd;

        if (rc == 1)
            break;
        if (rc != 0)
            continue;

        build_log_path(item.container_id, path, sizeof(path));
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log");
            continue;
        }

        if (write_fully(fd, item.data, item.length) < 0)
            perror("write log");

        close(fd);
    }

    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = arg;
    size_t host_len;

    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }

    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }

    close(cfg->log_write_fd);

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount private");

    host_len = strnlen(cfg->id, sizeof(cfg->id));
    if (host_len > 0 && sethostname(cfg->id, host_len) < 0)
        perror("sethostname");

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 1;
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void *producer_thread(void *arg)
{
    producer_ctx_t *producer = arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, producer->container_id, sizeof(item.container_id) - 1);

    while (1) {
        ssize_t rd = read(producer->read_fd, item.data, sizeof(item.data));

        if (rd == 0)
            break;
        if (rd < 0) {
            if (errno == EINTR)
                continue;
            perror("read log pipe");
            break;
        }

        item.length = (size_t)rd;
        if (bounded_buffer_push(&producer->ctx->log_buffer, &item) != 0)
            break;
    }

    close(producer->read_fd);
    return NULL;
}

static int create_control_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, CONTROL_BACKLOG) < 0) {
        close(fd);
        unlink(CONTROL_PATH);
        return -1;
    }

    return fd;
}

static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            container_record_t **out_record,
                            control_response_t *resp)
{
    container_record_t *record = NULL;
    child_config_t *child_cfg = NULL;
    producer_ctx_t *producer = NULL;
    void *stack = NULL;
    int pipefd[2] = { -1, -1 };
    int log_fd = -1;
    int clone_flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    pid_t child_pid;
    int rc;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        response_set(resp, 1, 1, "container '%s' already exists", req->container_id);
        return -1;
    }

    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        response_set(resp, 1, 1, "rootfs '%s' is already in use by a live container", req->rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ensure_directory(LOG_DIR, 0755) < 0) {
        response_set(resp, 1, 1, "failed to create %s: %s", LOG_DIR, strerror(errno));
        return -1;
    }

    record = calloc(1, sizeof(*record));
    child_cfg = calloc(1, sizeof(*child_cfg));
    producer = calloc(1, sizeof(*producer));
    stack = malloc(STACK_SIZE);

    if (!record || !child_cfg || !producer || !stack) {
        response_set(resp, 1, 1, "allocation failure while launching container");
        goto fail;
    }

    if (pthread_cond_init(&record->state_changed, NULL) != 0) {
        response_set(resp, 1, 1, "failed to initialize container wait state");
        goto fail;
    }

    strncpy(record->id, req->container_id, sizeof(record->id) - 1);
    strncpy(record->rootfs, req->rootfs, sizeof(record->rootfs) - 1);
    strncpy(record->command, req->command, sizeof(record->command) - 1);
    record->state = CONTAINER_STARTING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    record->nice_value = req->nice_value;
    snprintf(record->reason, sizeof(record->reason), "starting");
    build_log_path(record->id, record->log_path, sizeof(record->log_path));
    record->child_stack = stack;
    record->producer_ctx = producer;

    log_fd = open(record->log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (log_fd < 0) {
        response_set(resp, 1, 1, "failed to create log file %s: %s", record->log_path, strerror(errno));
        goto fail;
    }
    close(log_fd);
    log_fd = -1;

    if (pipe(pipefd) < 0) {
        response_set(resp, 1, 1, "pipe failed: %s", strerror(errno));
        goto fail;
    }

    strncpy(child_cfg->id, req->container_id, sizeof(child_cfg->id) - 1);
    strncpy(child_cfg->rootfs, req->rootfs, sizeof(child_cfg->rootfs) - 1);
    strncpy(child_cfg->command, req->command, sizeof(child_cfg->command) - 1);
    child_cfg->nice_value = req->nice_value;
    child_cfg->log_write_fd = pipefd[1];

    child_pid = clone(child_fn,
                      (char *)stack + STACK_SIZE,
                      clone_flags,
                      child_cfg);
    if (child_pid < 0) {
        response_set(resp, 1, 1, "clone failed: %s", strerror(errno));
        goto fail;
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    record->host_pid = child_pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    snprintf(record->reason, sizeof(record->reason), "running");

    producer->ctx = ctx;
    producer->read_fd = pipefd[0];
    strncpy(producer->container_id, record->id, sizeof(producer->container_id) - 1);

    rc = pthread_create(&record->producer_thread, NULL, producer_thread, producer);
    if (rc != 0) {
        errno = rc;
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        response_set(resp, 1, 1, "failed to start log producer thread: %s", strerror(errno));
        goto fail;
    }
    record->producer_started = 1;
    pipefd[0] = -1;

    if (ctx->monitor_fd >= 0 &&
        register_with_monitor(ctx->monitor_fd,
                              record->id,
                              record->host_pid,
                              record->soft_limit_bytes,
                              record->hard_limit_bytes) < 0) {
        fprintf(stderr,
                "warning: failed to register %s with monitor: %s\n",
                record->id,
                strerror(errno));
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    response_set(resp,
                 0,
                 0,
                 "started container=%s pid=%d log=%s soft=%luMiB hard=%luMiB nice=%d",
                 record->id,
                 record->host_pid,
                 record->log_path,
                 record->soft_limit_bytes >> 20,
                 record->hard_limit_bytes >> 20,
                 record->nice_value);

    free(child_cfg);
    *out_record = record;
    return 0;

fail:
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    if (log_fd >= 0)
        close(log_fd);
    if (record) {
        pthread_cond_destroy(&record->state_changed);
        free(record);
    }
    free(child_cfg);
    free(producer);
    free(stack);
    return -1;
}

static void mark_container_exit_locked(container_record_t *record, int status)
{
    record->exited = 1;

    if (WIFEXITED(status)) {
        record->exit_code = WEXITSTATUS(status);
        record->exit_signal = 0;
        if (record->stop_requested) {
            record->state = CONTAINER_STOPPED;
            snprintf(record->reason, sizeof(record->reason), "stopped");
        } else {
            record->state = CONTAINER_EXITED;
            snprintf(record->reason, sizeof(record->reason), "exited");
        }
        return;
    }

    if (WIFSIGNALED(status)) {
        record->exit_signal = WTERMSIG(status);
        record->exit_code = 128 + record->exit_signal;
        if (record->stop_requested) {
            record->state = CONTAINER_STOPPED;
            snprintf(record->reason, sizeof(record->reason), "stopped");
        } else if (record->exit_signal == SIGKILL) {
            record->state = CONTAINER_KILLED;
            snprintf(record->reason, sizeof(record->reason), "hard_limit_killed");
        } else {
            record->state = CONTAINER_KILLED;
            snprintf(record->reason, sizeof(record->reason), "signaled");
        }
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    while (1) {
        container_record_t *record;
        pthread_t producer_thread_id = 0;
        int should_join_producer = 0;
        void *stack_to_free = NULL;
        pid_t pid;
        int status;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_by_pid_locked(ctx, pid);
        if (record != NULL) {
            mark_container_exit_locked(record, status);
            if (record->producer_started && !record->producer_joined) {
                producer_thread_id = record->producer_thread;
                should_join_producer = 1;
                record->producer_joined = 1;
            }
            stack_to_free = record->child_stack;
            record->child_stack = NULL;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (record == NULL)
            continue;

        if (ctx->monitor_fd >= 0 &&
            unregister_from_monitor(ctx->monitor_fd, record->id, record->host_pid) < 0 &&
            errno != ENOENT) {
            fprintf(stderr,
                    "warning: failed to unregister %s from monitor: %s\n",
                    record->id,
                    strerror(errno));
        }

        if (should_join_producer)
            pthread_join(producer_thread_id, NULL);

        free(stack_to_free);

        pthread_mutex_lock(&ctx->metadata_lock);
        pthread_cond_broadcast(&record->state_changed);
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int any_live_containers(supervisor_ctx_t *ctx)
{
    container_record_t *cursor;
    int found = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        if (is_live_state(cursor->state)) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    return found;
}

static void request_live_container_shutdown(supervisor_ctx_t *ctx, int signal_value, int set_stop_requested)
{
    container_record_t *cursor;

    pthread_mutex_lock(&ctx->metadata_lock);
    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        if (!is_live_state(cursor->state))
            continue;
        if (set_stop_requested)
            cursor->stop_requested = 1;
        kill(cursor->host_pid, signal_value);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void shutdown_containers(supervisor_ctx_t *ctx)
{
    int i;

    request_live_container_shutdown(ctx, SIGTERM, 1);

    for (i = 0; i < SHUTDOWN_GRACE_TICKS; i++) {
        reap_children(ctx);
        if (!any_live_containers(ctx))
            return;
        usleep(WAIT_TICK_USEC);
    }

    request_live_container_shutdown(ctx, SIGKILL, 1);

    for (i = 0; i < SHUTDOWN_GRACE_TICKS; i++) {
        reap_children(ctx);
        if (!any_live_containers(ctx))
            return;
        usleep(WAIT_TICK_USEC);
    }
}

static void free_container_records(supervisor_ctx_t *ctx)
{
    container_record_t *cursor = ctx->containers;

    while (cursor != NULL) {
        container_record_t *next = cursor->next;

        if (cursor->producer_started && !cursor->producer_joined)
            pthread_join(cursor->producer_thread, NULL);

        if (cursor->producer_ctx != NULL)
            free(cursor->producer_ctx);
        if (cursor->child_stack != NULL)
            free(cursor->child_stack);

        pthread_cond_destroy(&cursor->state_changed);
        free(cursor);
        cursor = next;
    }

    ctx->containers = NULL;
}

static int build_ps_output(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *cursor;
    size_t used = 0;

    response_set(resp, 0, 0, "");

    append_format(resp->message,
                  sizeof(resp->message),
                  &used,
                  "ID\tPID\tSTATE\tREASON\tSOFT(MiB)\tHARD(MiB)\tNICE\tEXIT\tSTARTED\tLOG\n");

    pthread_mutex_lock(&ctx->metadata_lock);
    for (cursor = ctx->containers; cursor != NULL; cursor = cursor->next) {
        char started_at[64];
        char exit_info[32];

        format_start_time(cursor->started_at, started_at, sizeof(started_at));
        if (!cursor->exited)
            snprintf(exit_info, sizeof(exit_info), "-");
        else if (cursor->exit_signal != 0)
            snprintf(exit_info, sizeof(exit_info), "sig:%d", cursor->exit_signal);
        else
            snprintf(exit_info, sizeof(exit_info), "%d", cursor->exit_code);

        if (append_format(resp->message,
                          sizeof(resp->message),
                          &used,
                          "%s\t%d\t%s\t%s\t%lu\t%lu\t%d\t%s\t%s\t%s\n",
                          cursor->id,
                          cursor->host_pid,
                          state_to_string(cursor->state),
                          cursor->reason,
                          cursor->soft_limit_bytes >> 20,
                          cursor->hard_limit_bytes >> 20,
                          cursor->nice_value,
                          exit_info,
                          started_at,
                          cursor->log_path) != 0) {
            append_format(resp->message,
                          sizeof(resp->message),
                          &used,
                          "... output truncated ...\n");
            break;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (used == 0)
        response_set(resp, 0, 0, "no tracked containers");

    return 0;
}

static int build_logs_output(const char *path, control_response_t *resp)
{
    int fd;
    off_t size;
    off_t start;
    ssize_t rd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        response_set(resp, 1, 1, "unable to open %s: %s", path, strerror(errno));
        return -1;
    }

    size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        close(fd);
        response_set(resp, 1, 1, "unable to stat log %s: %s", path, strerror(errno));
        return -1;
    }

    start = 0;
    if (size > (off_t)(sizeof(resp->message) - 1))
        start = size - (off_t)(sizeof(resp->message) - 1);

    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        response_set(resp, 1, 1, "unable to seek log %s: %s", path, strerror(errno));
        return -1;
    }

    rd = read(fd, resp->message, sizeof(resp->message) - 1);
    close(fd);
    if (rd < 0) {
        response_set(resp, 1, 1, "unable to read %s: %s", path, strerror(errno));
        return -1;
    }

    resp->status = 0;
    resp->exit_status = 0;
    resp->message[rd] = '\0';
    return 0;
}

static void *run_wait_thread(void *arg)
{
    run_wait_ctx_t *wait_ctx = arg;
    control_response_t resp;

    pthread_mutex_lock(&wait_ctx->ctx->metadata_lock);
    while (!wait_ctx->container->exited)
        pthread_cond_wait(&wait_ctx->container->state_changed, &wait_ctx->ctx->metadata_lock);

    response_set(&resp,
                 0,
                 wait_ctx->container->exit_code,
                 "container=%s finished state=%s reason=%s exit_status=%d log=%s",
                 wait_ctx->container->id,
                 state_to_string(wait_ctx->container->state),
                 wait_ctx->container->reason,
                 wait_ctx->container->exit_code,
                 wait_ctx->container->log_path);
    pthread_mutex_unlock(&wait_ctx->ctx->metadata_lock);

    send_all(wait_ctx->client_fd, &resp, sizeof(resp));
    close(wait_ctx->client_fd);
    free(wait_ctx);
    return NULL;
}

static int handle_stop_request(supervisor_ctx_t *ctx,
                               const control_request_t *req,
                               control_response_t *resp)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, req->container_id);
    if (record == NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        response_set(resp, 1, 1, "unknown container '%s'", req->container_id);
        return -1;
    }

    if (!is_live_state(record->state)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        response_set(resp, 1, 1, "container '%s' is not running", req->container_id);
        return -1;
    }

    record->stop_requested = 1;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(record->host_pid, SIGTERM) < 0) {
        response_set(resp, 1, 1, "failed to signal %s: %s", req->container_id, strerror(errno));
        return -1;
    }

    response_set(resp, 0, 0, "stop requested for container=%s pid=%d", record->id, record->host_pid);
    return 0;
}

static int handle_control_request(supervisor_ctx_t *ctx,
                                  int client_fd,
                                  const control_request_t *req,
                                  int *client_owned_by_thread)
{
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));
    *client_owned_by_thread = 0;

    switch (req->kind) {
    case CMD_START:
    {
        container_record_t *record = NULL;
        launch_container(ctx, req, &record, &resp);
        break;
    }
    case CMD_RUN:
    {
        container_record_t *record = NULL;
        run_wait_ctx_t *wait_ctx = NULL;
        pthread_t waiter;
        int rc;

        if (launch_container(ctx, req, &record, &resp) != 0)
            break;

        wait_ctx = calloc(1, sizeof(*wait_ctx));
        if (wait_ctx == NULL) {
            response_set(&resp, 1, 1, "failed to allocate run waiter");
            break;
        }

        wait_ctx->ctx = ctx;
        wait_ctx->container = record;
        wait_ctx->client_fd = client_fd;

        rc = pthread_create(&waiter, NULL, run_wait_thread, wait_ctx);
        if (rc != 0) {
            errno = rc;
            pthread_mutex_lock(&ctx->metadata_lock);
            record->stop_requested = 1;
            pthread_mutex_unlock(&ctx->metadata_lock);
            kill(record->host_pid, SIGTERM);
            free(wait_ctx);
            response_set(&resp, 1, 1, "failed to create run waiter: %s", strerror(errno));
            break;
        }

        pthread_detach(waiter);
        *client_owned_by_thread = 1;
        return 0;
    }
    case CMD_PS:
        build_ps_output(ctx, &resp);
        break;
    case CMD_LOGS:
    {
        container_record_t *record;
        char log_path[PATH_MAX];

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_locked(ctx, req->container_id);
        if (record != NULL)
            strncpy(log_path, record->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (record == NULL) {
            response_set(&resp, 1, 1, "unknown container '%s'", req->container_id);
            break;
        }

        log_path[sizeof(log_path) - 1] = '\0';
        build_logs_output(log_path, &resp);
        break;
    }
    case CMD_STOP:
        handle_stop_request(ctx, req, &resp);
        break;
    default:
        response_set(&resp, 1, 1, "unsupported command");
        break;
    }

    send_all(client_fd, &resp, sizeof(resp));
    return 0;
}

static void drain_signal_pipe(supervisor_ctx_t *ctx)
{
    unsigned char buffer[64];

    while (read(ctx->signal_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.signal_pipe[0] = -1;
    ctx.signal_pipe[1] = -1;
    strncpy(ctx.base_rootfs, rootfs, sizeof(ctx.base_rootfs) - 1);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (pipe(ctx.signal_pipe) < 0) {
        perror("pipe");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fcntl(ctx.signal_pipe[0], F_SETFL, fcntl(ctx.signal_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(ctx.signal_pipe[1], F_SETFL, fcntl(ctx.signal_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    g_signal_pipe[0] = ctx.signal_pipe[0];
    g_signal_pipe[1] = ctx.signal_pipe[1];

    if (install_supervisor_signal_handlers() < 0) {
        perror("sigaction");
        close(ctx.signal_pipe[0]);
        close(ctx.signal_pipe[1]);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "warning: monitor unavailable at /dev/container_monitor: %s\n", strerror(errno));

    ctx.server_fd = create_control_socket();
    if (ctx.server_fd < 0) {
        perror("create_control_socket");
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        close(ctx.signal_pipe[0]);
        close(ctx.signal_pipe[1]);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        close(ctx.signal_pipe[0]);
        close(ctx.signal_pipe[1]);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fprintf(stderr,
            "supervisor running base-rootfs=%s control=%s logs=%s/\n",
            ctx.base_rootfs,
            CONTROL_PATH,
            LOG_DIR);

    while (!ctx.should_stop) {
        fd_set readfds;
        int maxfd;

        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        FD_SET(ctx.signal_pipe[0], &readfds);
        maxfd = (ctx.server_fd > ctx.signal_pipe[0]) ? ctx.server_fd : ctx.signal_pipe[0];

        rc = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (FD_ISSET(ctx.signal_pipe[0], &readfds))
            drain_signal_pipe(&ctx);

        if (g_supervisor_sigchld) {
            g_supervisor_sigchld = 0;
            reap_children(&ctx);
        }

        if (g_supervisor_stop)
            ctx.should_stop = 1;

        if (ctx.should_stop)
            break;

        if (FD_ISSET(ctx.server_fd, &readfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);

            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;
                perror("accept");
                continue;
            }

            while (1) {
                control_request_t req;
                int client_owned_by_thread = 0;

                memset(&req, 0, sizeof(req));
                if (recv_all(client_fd, &req, sizeof(req)) < 0) {
                    close(client_fd);
                    break;
                }

                handle_control_request(&ctx, client_fd, &req, &client_owned_by_thread);
                if (!client_owned_by_thread)
                    close(client_fd);
                break;
            }
        }
    }

    shutdown_containers(&ctx);
    reap_children(&ctx);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    close(ctx.signal_pipe[0]);
    close(ctx.signal_pipe[1]);

    free_container_records(&ctx);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

static int low_level_control_exchange(const control_request_t *req, control_response_t *resp)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (send_all(fd, req, sizeof(*req)) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    if (recv_all(fd, resp, sizeof(*resp)) < 0) {
        perror("recv");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int send_stop_for_run_client(void)
{
    control_request_t stop_req;
    control_response_t stop_resp;

    memset(&stop_req, 0, sizeof(stop_req));
    stop_req.kind = CMD_STOP;
    strncpy(stop_req.container_id, g_client_run_id, sizeof(stop_req.container_id) - 1);

    if (low_level_control_exchange(&stop_req, &stop_resp) != 0)
        return 1;

    fprintf(stderr, "%s\n", stop_resp.message);
    return stop_resp.status != 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (send_all(fd, req, sizeof(*req)) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_RUN) {
        struct sigaction old_int;
        struct sigaction old_term;
        char *cursor = (char *)&resp;
        size_t remaining = sizeof(resp);
        int stop_sent = 0;

        memset(&old_int, 0, sizeof(old_int));
        memset(&old_term, 0, sizeof(old_term));
        g_client_forward_stop = 0;
        memset(g_client_run_id, 0, sizeof(g_client_run_id));
        strncpy(g_client_run_id, req->container_id, sizeof(g_client_run_id) - 1);

        if (install_client_wait_handlers(&old_int, &old_term) < 0) {
            perror("sigaction");
            close(fd);
            return 1;
        }

        while (remaining > 0) {
            ssize_t rd = recv(fd, cursor, remaining, 0);

            if (rd == 0) {
                errno = ECONNRESET;
                perror("recv");
                restore_client_wait_handlers(&old_int, &old_term);
                close(fd);
                return 1;
            }

            if (rd < 0) {
                if (errno == EINTR) {
                    if (g_client_forward_stop && !stop_sent) {
                        stop_sent = 1;
                        send_stop_for_run_client();
                    }
                    continue;
                }

                perror("recv");
                restore_client_wait_handlers(&old_int, &old_term);
                close(fd);
                return 1;
            }

            cursor += rd;
            remaining -= (size_t)rd;
        }

        restore_client_wait_handlers(&old_int, &old_term);
        close(fd);
        printf("%s\n", resp.message);
        if (resp.status != 0)
            return 1;
        return resp.exit_status;
    }

    if (recv_all(fd, &resp, sizeof(resp)) < 0) {
        perror("recv");
        close(fd);
        return 1;
    }

    close(fd);
    printf("%s\n", resp.message);
    return resp.status != 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
