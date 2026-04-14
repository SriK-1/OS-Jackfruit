/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
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
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

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

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];

    int log_pipe_read_fd;
    pthread_t log_thread;

    struct container_record *next;
} container_record_t;

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
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    
    bounded_buffer_t log_buffer;
    pthread_t consumer_thread;
} supervisor_ctx_t;

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

/* ---------------- basic file/socket helpers ---------------- */

static int make_logs_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists but is not a directory\n", LOG_DIR);
            return -1;
        }
        return 0;
    }
    if (mkdir(LOG_DIR, 0755) < 0) {
        perror("mkdir logs");
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int setup_control_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_control_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ---------------- container metadata helpers ---------------- */

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            if (strncmp(cur->rootfs, rootfs, sizeof(cur->rootfs)) == 0)
                return 1;
        }
        cur = cur->next;
    }
    return 0;
}

/* ---------------- bounded buffer implementation ---------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
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
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ---------------- log consumer thread (supervisor side) ---------------- */

static void *logging_consumer_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write_all(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/* ---------------- log producer thread (per container) ---------------- */

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    int read_fd;
    bounded_buffer_t *buffer;
} log_producer_arg_t;

static void *log_pipe_producer_thread(void *arg)
{
    log_producer_arg_t *a = (log_producer_arg_t *)arg;
    log_item_t item;
    
    strncpy(item.container_id, a->container_id, sizeof(item.container_id) - 1);

    while (1) {
        ssize_t r = read(a->read_fd, item.data, LOG_CHUNK_SIZE);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        item.length = (size_t)r;
        
        if (bounded_buffer_push(a->buffer, &item) < 0) break;
    }

    close(a->read_fd);
    free(a);
    return NULL;
}

/* ---------------- child entrypoint ---------------- */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout+stderr to the logging pipe FIRST so errors are captured */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Now perform namespace setup */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount MS_PRIVATE");
        return 1;
    }

    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0) {
        perror("sethostname");
        return 1;
    }

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

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0) {
            perror("nice");
        }
    }

    char *const sh_argv[] = {"/bin/sh", "-c", cfg->command, NULL};
    execv("/bin/sh", sh_argv);

    perror("execv");
    return 127;
}

/* ---------------- SIGCHLD handling ---------------- */

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cur = ctx->containers;
        while (cur) {
            if (cur->host_pid == pid) {
                cur->state = CONTAINER_EXITED;
                if (WIFEXITED(status)) {
                    cur->exit_code = WEXITSTATUS(status);
                    cur->exit_signal = 0;
                } else if (WIFSIGNALED(status)) {
                    cur->exit_code = 0;
                    cur->exit_signal = WTERMSIG(status);
                }
                
                if (ctx->monitor_fd >= 0) {
                    struct monitor_request mreq;
                    memset(&mreq, 0, sizeof(mreq));
                    mreq.pid = cur->host_pid;
                    strncpy(mreq.container_id, cur->id, sizeof(mreq.container_id) - 1);
                    ioctl(ctx->monitor_fd, MONITOR_UNREGISTER, &mreq);
                }
                
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ---------------- start container ---------------- */

static int supervisor_start_container(supervisor_ctx_t *ctx,
                                      const control_request_t *req,
                                      control_response_t *resp)
{
    int pipefd[2] = {-1, -1};
    void *stack = NULL;
    pid_t pid;
    container_record_t *rec = NULL;
    child_config_t *child_cfg = NULL;

    pthread_mutex_lock(&ctx->metadata_lock);

    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "Container id already exists: %s\n", req->container_id);
        return -1;
    }

    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Rootfs already in use by another running container: %s\n", req->rootfs);
        return -1;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "pipe failed: %s\n", strerror(errno));
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "malloc stack failed\n");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    child_cfg = calloc(1, sizeof(*child_cfg));
    if (!child_cfg) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "calloc child cfg failed\n");
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    strncpy(child_cfg->id, req->container_id, sizeof(child_cfg->id) - 1);
    strncpy(child_cfg->rootfs, req->rootfs, sizeof(child_cfg->rootfs) - 1);
    strncpy(child_cfg->command, req->command, sizeof(child_cfg->command) - 1);
    child_cfg->nice_value = req->nice_value;
    child_cfg->log_write_fd = pipefd[1];

    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "calloc container record failed\n");
        free(child_cfg);
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    rec->started_at = time(NULL);
    rec->state = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);
    rec->log_pipe_read_fd = pipefd[0];

    /* Ensure the file exists immediately so 'logs' command doesn't fail if empty */
    int init_fd = open(rec->log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (init_fd >= 0) close(init_fd);

    int flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;

    pid = clone(child_fn, (char *)stack + STACK_SIZE, flags, child_cfg);
    if (pid < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "clone failed: %s\n", strerror(errno));
        free(rec);
        free(child_cfg);
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
        struct monitor_request mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.pid = rec->host_pid;
        mreq.soft_limit_bytes = rec->soft_limit_bytes;
        mreq.hard_limit_bytes = rec->hard_limit_bytes;
        strncpy(mreq.container_id, rec->id, sizeof(mreq.container_id) - 1);
        ioctl(ctx->monitor_fd, MONITOR_REGISTER, &mreq);
    }

    log_producer_arg_t *lpa = calloc(1, sizeof(*lpa));
    if (lpa) {
        strncpy(lpa->container_id, rec->id, sizeof(lpa->container_id) - 1);
        lpa->read_fd = rec->log_pipe_read_fd;
        lpa->buffer = &ctx->log_buffer;
        pthread_create(&rec->log_thread, NULL, log_pipe_producer_thread, lpa);
    }

    (void)stack;
    (void)child_cfg;

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Started container id=%s pid=%d log=%s\n", rec->id, rec->host_pid, rec->log_path);
    return 0;
}

/* ---------------- supervisor loop ---------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    if (make_logs_dir() < 0) {
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    bounded_buffer_init(&ctx.log_buffer);
    pthread_create(&ctx.consumer_thread, NULL, logging_consumer_thread, &ctx);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/container_monitor: %s\n", strerror(errno));
    }

    ctx.server_fd = setup_control_socket();
    if (ctx.server_fd < 0) {
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fprintf(stderr, "Supervisor started. base-rootfs=%s control=%s\n", rootfs, CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd;
        control_request_t req;
        control_response_t resp;

        reap_children(&ctx);

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        resp.status = 0;

        if (read_all(client_fd, &req, sizeof(req)) < 0) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "Bad request (read failed)\n");
            (void)write_all(client_fd, &resp, sizeof(resp));
            close(client_fd);
            continue;
        }

        switch (req.kind) {
        case CMD_PS: {
            char tmp[CONTROL_MESSAGE_LEN];
            size_t used = 0;

            used += (size_t)snprintf(resp.message + used, sizeof(resp.message) - used,
                                     "ID\tPID\tSTATE\tEXIT\tLOG\n");

            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *cur = ctx.containers;
            if (!cur) {
                used += (size_t)snprintf(resp.message + used, sizeof(resp.message) - used,
                                         "(none)\n");
            }
            while (cur && used < sizeof(resp.message)) {
                if (cur->state == CONTAINER_EXITED) {
                    if (cur->exit_signal) {
                        snprintf(tmp, sizeof(tmp), "sig=%d", cur->exit_signal);
                    } else {
                        snprintf(tmp, sizeof(tmp), "code=%d", cur->exit_code);
                    }
                } else {
                    snprintf(tmp, sizeof(tmp), "-");
                }

                used += (size_t)snprintf(resp.message + used, sizeof(resp.message) - used,
                                         "%s\t%d\t%s\t%s\t%s\n",
                                         cur->id, cur->host_pid, state_to_string(cur->state),
                                         tmp, cur->log_path);
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            resp.status = 0;
            break;
        }

        case CMD_START:
            (void)supervisor_start_container(&ctx, &req, &resp);
            break;

        case CMD_STOP: {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *cur = find_container_locked(&ctx, req.container_id);
            if (cur && (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING)) {
                kill(cur->host_pid, SIGTERM);
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "Sent SIGTERM to container %s\n", cur->id);
            } else {
                resp.status = 1;
                snprintf(resp.message, sizeof(resp.message), "Container %s not found or not running\n", req.container_id);
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            break;
        }

        default:
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "Command not fully implemented yet for IPC.\n");
            break;
        }

        (void)write_all(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.consumer_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/* ---------------- client path ---------------- */

static int send_control_request(const control_request_t *req)
{
    int fd;
    control_response_t resp;

    fd = connect_control_socket();
    if (fd < 0)
        return 1;

    if (write_all(fd, req, sizeof(*req)) < 0) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read_all(fd, &resp, sizeof(resp)) < 0) {
        perror("read response");
        close(fd);
        return 1;
    }

    close(fd);

    if (resp.message[0] != '\0')
        printf("%s", resp.message);

    return resp.status;
}

/* ---------------- command handlers ---------------- */

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
    fprintf(stderr, "Run blocking semantics will be added in a future checkpoint.\n");
    return 1;
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
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, argv[2]);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open log file for %s. Ensure the container has started.\n", argv[2]);
        return 1;
    }

    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)r);
    }
    
    close(fd);
    return 0;
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
