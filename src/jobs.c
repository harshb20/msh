#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_EVENT_CAPACITY 4096

typedef struct {
    pid_t pid;
    int status;
} ChildEvent;

static int signal_pipe[2] = { -1, -1 };
static ChildEvent child_events[CHILD_EVENT_CAPACITY];
static volatile sig_atomic_t event_head;
static volatile sig_atomic_t event_tail;
static volatile sig_atomic_t event_overflow;

static void sigchld_handler(int signal_number)
{
    (void)signal_number;
    int saved_errno = errno;

    for (;;) {
        sig_atomic_t head = event_head;
        sig_atomic_t next = (head + 1) % CHILD_EVENT_CAPACITY;
        if (next == event_tail) {
            event_overflow = 1;
            break;
        }

        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) {
            break;
        }
        child_events[head].pid = pid;
        child_events[head].status = status;
        event_head = next;
    }

    if (signal_pipe[1] >= 0) {
        const unsigned char byte = 1;
        ssize_t ignored = write(signal_pipe[1], &byte, sizeof(byte));
        (void)ignored;
    }
    errno = saved_errno;
}

static int set_fd_flags(int fd)
{
    int status_flags = fcntl(fd, F_GETFL);
    if (status_flags < 0 || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return -1;
    }
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}

int jobs_initialize(Shell *shell)
{
    shell->jobs = NULL;
    shell->next_job_id = 1;
    event_head = 0;
    event_tail = 0;
    event_overflow = 0;

    if (pipe(signal_pipe) < 0 || set_fd_flags(signal_pipe[0]) < 0 ||
        set_fd_flags(signal_pipe[1]) < 0) {
        perror(MSH_NAME ": signal pipe");
        if (signal_pipe[0] >= 0) {
            close(signal_pipe[0]);
        }
        if (signal_pipe[1] >= 0) {
            close(signal_pipe[1]);
        }
        signal_pipe[0] = -1;
        signal_pipe[1] = -1;
        return -1;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigchld_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        perror(MSH_NAME ": sigaction");
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        signal_pipe[0] = -1;
        signal_pipe[1] = -1;
        return -1;
    }
    return 0;
}

void jobs_block_sigchld(sigset_t *old_mask)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &mask, old_mask) < 0) {
        perror(MSH_NAME ": sigprocmask");
    }
}

void jobs_restore_sigmask(const sigset_t *old_mask)
{
    if (sigprocmask(SIG_SETMASK, old_mask, NULL) < 0) {
        perror(MSH_NAME ": sigprocmask");
    }
}

int jobs_signal_fd(void)
{
    return signal_pipe[0];
}

static Job *find_job_by_pid(Shell *shell, pid_t pid, Process **process_out)
{
    for (Job *job = shell->jobs; job != NULL; job = job->next) {
        for (Process *process = job->processes; process != NULL;
             process = process->next) {
            if (process->pid == pid) {
                if (process_out != NULL) {
                    *process_out = process;
                }
                return job;
            }
        }
    }
    return NULL;
}

static void recompute_job_state(Job *job)
{
    bool all_completed = true;
    bool all_completed_or_stopped = true;
    bool any_stopped = false;

    for (Process *process = job->processes; process != NULL;
         process = process->next) {
        if (!process->completed) {
            all_completed = false;
        }
        if (!process->completed && !process->stopped) {
            all_completed_or_stopped = false;
        }
        if (process->stopped) {
            any_stopped = true;
        }
    }

    JobState old_state = job->state;
    if (all_completed) {
        job->state = JOB_DONE;
    } else if (all_completed_or_stopped && any_stopped) {
        job->state = JOB_STOPPED;
    } else {
        job->state = JOB_RUNNING;
    }
    if (job->state != old_state) {
        job->notified = false;
    }
}

static void apply_child_status(Shell *shell, pid_t pid, int status)
{
    Process *process = NULL;
    Job *job = find_job_by_pid(shell, pid, &process);
    if (job == NULL || process == NULL) {
        return;
    }

    process->status = status;
    if (WIFSTOPPED(status)) {
        process->stopped = true;
    } else if (WIFCONTINUED(status)) {
        process->stopped = false;
    } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        process->completed = true;
        process->stopped = false;
    }
    recompute_job_state(job);
}

static void drain_signal_pipe(void)
{
    if (signal_pipe[0] < 0) {
        return;
    }
    unsigned char buffer[128];
    while (read(signal_pipe[0], buffer, sizeof(buffer)) > 0) {
    }
}

void jobs_process_events(Shell *shell)
{
    sigset_t old_mask;
    jobs_block_sigchld(&old_mask);
    drain_signal_pipe();

    while (event_tail != event_head) {
        sig_atomic_t tail = event_tail;
        ChildEvent event = child_events[tail];
        event_tail = (tail + 1) % CHILD_EVENT_CAPACITY;
        apply_child_status(shell, event.pid, event.status);
    }

    if (event_overflow) {
        event_overflow = 0;
        for (;;) {
            int status = 0;
            pid_t pid = waitpid(-1, &status,
                                WNOHANG | WUNTRACED | WCONTINUED);
            if (pid <= 0) {
                break;
            }
            apply_child_status(shell, pid, status);
        }
    }
    jobs_restore_sigmask(&old_mask);
}

Job *job_create(Shell *shell, const char *command, bool background)
{
    Job *job = xcalloc(1, sizeof(*job));
    job->id = shell->next_job_id++;
    job->command = xstrdup(command);
    job->background = background;
    job->state = JOB_RUNNING;

    if (shell->jobs == NULL) {
        shell->jobs = job;
    } else {
        Job *tail = shell->jobs;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = job;
    }
    return job;
}

void job_add_process(Job *job, pid_t pid)
{
    Process *process = xcalloc(1, sizeof(*process));
    process->pid = pid;
    job->last_pid = pid;
    if (job->processes == NULL) {
        job->processes = process;
    } else {
        Process *tail = job->processes;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = process;
    }
}

void job_remove(Shell *shell, Job *job)
{
    Job **link = &shell->jobs;
    while (*link != NULL && *link != job) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return;
    }
    *link = job->next;

    Process *process = job->processes;
    while (process != NULL) {
        Process *next = process->next;
        free(process);
        process = next;
    }
    free(job->command);
    free(job);
}

static int parse_job_id(const char *spec)
{
    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "%+") == 0 ||
        strcmp(spec, "%%") == 0) {
        return -1;
    }
    if (spec[0] == '%') {
        ++spec;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(spec, &end, 10);
    if (errno != 0 || end == spec || *end != '\0' || value <= 0 ||
        value > 2147483647L) {
        return -2;
    }
    return (int)value;
}

Job *job_find(Shell *shell, const char *spec)
{
    int wanted = parse_job_id(spec);
    if (wanted == -2) {
        return NULL;
    }

    Job *selected = NULL;
    for (Job *job = shell->jobs; job != NULL; job = job->next) {
        if (job->state == JOB_DONE) {
            continue;
        }
        if (wanted < 0) {
            if (selected == NULL || job->id > selected->id) {
                selected = job;
            }
        } else if (job->id == wanted) {
            return job;
        }
    }
    return selected;
}

const char *job_state_name(JobState state)
{
    switch (state) {
    case JOB_RUNNING: return "Running";
    case JOB_STOPPED: return "Stopped";
    case JOB_DONE: return "Done";
    }
    return "Unknown";
}

int job_last_status(const Job *job)
{
    for (const Process *process = job->processes; process != NULL;
         process = process->next) {
        if (process->pid != job->last_pid) {
            continue;
        }
        if (WIFEXITED(process->status)) {
            return WEXITSTATUS(process->status);
        }
        if (WIFSIGNALED(process->status)) {
            return 128 + WTERMSIG(process->status);
        }
        if (WIFSTOPPED(process->status)) {
            return 128 + WSTOPSIG(process->status);
        }
    }
    return 0;
}

static int job_last_signal(const Job *job)
{
    for (const Process *process = job->processes; process != NULL;
         process = process->next) {
        if (process->pid == job->last_pid && WIFSIGNALED(process->status)) {
            return WTERMSIG(process->status);
        }
    }
    return 0;
}

void jobs_notify(Shell *shell)
{
    jobs_process_events(shell);
    Job *job = shell->jobs;
    while (job != NULL) {
        Job *next = job->next;
        if (job->background && !job->notified && job->state != JOB_RUNNING) {
            fprintf(stderr, "[%d]+  %-8s %s\n", job->id,
                    job_state_name(job->state), job->command);
            job->notified = true;
        }
        if (job->background && job->state == JOB_DONE && job->notified) {
            job_remove(shell, job);
        }
        job = next;
    }
}

int job_wait_foreground(Shell *shell, Job *job, bool send_continue)
{
    job->background = false;
    job->notified = false;

    if (shell->interactive) {
        if (tcsetpgrp(shell->terminal_fd, job->pgid) < 0 && errno != ENOTTY) {
            perror(MSH_NAME ": tcsetpgrp");
        }
        if (send_continue) {
            (void)tcsetattr(shell->terminal_fd, TCSADRAIN, &job->tmodes);
        }
    }
    if (send_continue && kill(-job->pgid, SIGCONT) < 0) {
        perror(MSH_NAME ": SIGCONT");
    }
    if (send_continue) {
        for (Process *process = job->processes; process != NULL;
             process = process->next) {
            process->stopped = false;
        }
        job->state = JOB_RUNNING;
    }

    sigset_t old_mask;
    jobs_block_sigchld(&old_mask);
    for (;;) {
        jobs_process_events(shell);
        if (job->state != JOB_RUNNING) {
            break;
        }
        sigset_t suspend_mask = old_mask;
        sigdelset(&suspend_mask, SIGCHLD);
        (void)sigsuspend(&suspend_mask);
    }
    jobs_restore_sigmask(&old_mask);

    if (shell->interactive) {
        if (job->state == JOB_STOPPED) {
            (void)tcgetattr(shell->terminal_fd, &job->tmodes);
        }
        if (tcsetpgrp(shell->terminal_fd, shell->shell_pgid) < 0 &&
            errno != ENOTTY) {
            perror(MSH_NAME ": tcsetpgrp");
        }
        (void)tcsetattr(shell->terminal_fd, TCSADRAIN, &shell->shell_tmodes);
    }

    int status = job_last_status(job);
    int terminating_signal = job_last_signal(job);
    if (job->state == JOB_DONE && terminating_signal != 0 &&
        terminating_signal != SIGINT && terminating_signal != SIGPIPE) {
        const char *description = strsignal(terminating_signal);
        fprintf(stderr, "%s\n", description == NULL ? "Terminated"
                                                      : description);
    }
    if (job->state == JOB_STOPPED) {
        job->background = true;
        fprintf(stderr, "[%d]+  Stopped  %s\n", job->id, job->command);
        job->notified = true;
    } else if (job->state == JOB_DONE) {
        job_remove(shell, job);
    }
    return status;
}

void jobs_shutdown(Shell *shell)
{
    for (Job *job = shell->jobs; job != NULL; job = job->next) {
        if (job->state != JOB_DONE && job->pgid > 0) {
            (void)kill(-job->pgid, SIGHUP);
            if (job->state == JOB_STOPPED) {
                (void)kill(-job->pgid, SIGCONT);
            }
        }
    }
    jobs_process_events(shell);
    while (shell->jobs != NULL) {
        job_remove(shell, shell->jobs);
    }
    if (signal_pipe[0] >= 0) {
        close(signal_pipe[0]);
    }
    if (signal_pipe[1] >= 0) {
        close(signal_pipe[1]);
    }
    signal_pipe[0] = -1;
    signal_pipe[1] = -1;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGCHLD, &action, NULL);
}
