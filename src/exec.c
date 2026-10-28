#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    RedirType type;
    char *target;
} ExpandedRedirection;

typedef struct {
    char **argv;
    size_t argc;
    size_t argv_capacity;
    ExpandedRedirection *redirs;
    size_t redir_count;
} ExpandedCommand;

typedef struct {
    ExpandedCommand *commands;
    size_t count;
} ExpandedPipeline;

static void expanded_command_add_arg(ExpandedCommand *command, char *argument)
{
    if (command->argc + 1 >= command->argv_capacity) {
        size_t capacity = command->argv_capacity == 0 ? 8
                                                       : command->argv_capacity * 2;
        command->argv = xrealloc(command->argv,
                                 capacity * sizeof(*command->argv));
        command->argv_capacity = capacity;
    }
    command->argv[command->argc++] = argument;
    command->argv[command->argc] = NULL;
}

static void expanded_command_free(ExpandedCommand *command)
{
    for (size_t i = 0; i < command->argc; ++i) {
        free(command->argv[i]);
    }
    for (size_t i = 0; i < command->redir_count; ++i) {
        free(command->redirs[i].target);
    }
    free(command->argv);
    free(command->redirs);
    memset(command, 0, sizeof(*command));
}

static void expanded_pipeline_free(ExpandedPipeline *pipeline)
{
    for (size_t i = 0; i < pipeline->count; ++i) {
        expanded_command_free(&pipeline->commands[i]);
    }
    free(pipeline->commands);
    memset(pipeline, 0, sizeof(*pipeline));
}

static int expand_command(const Shell *shell, const Command *source,
                          ExpandedCommand *target)
{
    memset(target, 0, sizeof(*target));
    for (size_t i = 0; i < source->word_count; ++i) {
        StringVec fields = { 0 };
        char *error_message = NULL;
        if (expand_word(shell, source->words[i], &fields, &error_message) < 0) {
            fprintf(stderr, "%s: %s\n", MSH_NAME, error_message);
            free(error_message);
            string_vec_free(&fields);
            return -1;
        }
        for (size_t j = 0; j < fields.count; ++j) {
            expanded_command_add_arg(target, fields.items[j]);
            fields.items[j] = NULL;
        }
        string_vec_free(&fields);
    }

    target->redir_count = source->redir_count;
    target->redirs = xcalloc(target->redir_count, sizeof(*target->redirs));
    for (size_t i = 0; i < source->redir_count; ++i) {
        target->redirs[i].type = source->redirs[i].type;
        if (source->redirs[i].type == REDIR_ERR_TO_OUT) {
            continue;
        }
        StringVec fields = { 0 };
        char *error_message = NULL;
        if (expand_word(shell, source->redirs[i].target, &fields,
                        &error_message) < 0) {
            fprintf(stderr, "%s: %s\n", MSH_NAME, error_message);
            free(error_message);
            string_vec_free(&fields);
            return -1;
        }
        if (fields.count != 1) {
            fprintf(stderr, "%s: ambiguous redirect\n", MSH_NAME);
            string_vec_free(&fields);
            return -1;
        }
        target->redirs[i].target = fields.items[0];
        fields.items[0] = NULL;
        string_vec_free(&fields);
    }
    return 0;
}

static int expand_pipeline(const Shell *shell, const Pipeline *source,
                           ExpandedPipeline *target)
{
    memset(target, 0, sizeof(*target));
    target->count = source->count;
    target->commands = xcalloc(target->count, sizeof(*target->commands));
    for (size_t i = 0; i < source->count; ++i) {
        if (expand_command(shell, &source->commands[i],
                           &target->commands[i]) < 0) {
            expanded_pipeline_free(target);
            return -1;
        }
    }
    return 0;
}

static int open_redirection(const char *path, int flags)
{
    int fd = open(path, flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", MSH_NAME, path, strerror(errno));
    }
    return fd;
}

static int duplicate_and_close(int fd, int destination)
{
    if (dup2(fd, destination) < 0) {
        fprintf(stderr, "%s: dup2: %s\n", MSH_NAME, strerror(errno));
        close(fd);
        return -1;
    }
    if (fd != destination) {
        close(fd);
    }
    return 0;
}

static int apply_redirections(const ExpandedCommand *command)
{
    for (size_t i = 0; i < command->redir_count; ++i) {
        const ExpandedRedirection *redirection = &command->redirs[i];
        int fd;
        switch (redirection->type) {
        case REDIR_IN:
            fd = open_redirection(redirection->target, O_RDONLY);
            if (fd < 0 || duplicate_and_close(fd, STDIN_FILENO) < 0) return -1;
            break;
        case REDIR_OUT:
            fd = open_redirection(redirection->target,
                                  O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0 || duplicate_and_close(fd, STDOUT_FILENO) < 0) return -1;
            break;
        case REDIR_APPEND:
            fd = open_redirection(redirection->target,
                                  O_WRONLY | O_CREAT | O_APPEND);
            if (fd < 0 || duplicate_and_close(fd, STDOUT_FILENO) < 0) return -1;
            break;
        case REDIR_ERR:
            fd = open_redirection(redirection->target,
                                  O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0 || duplicate_and_close(fd, STDERR_FILENO) < 0) return -1;
            break;
        case REDIR_ERR_APPEND:
            fd = open_redirection(redirection->target,
                                  O_WRONLY | O_CREAT | O_APPEND);
            if (fd < 0 || duplicate_and_close(fd, STDERR_FILENO) < 0) return -1;
            break;
        case REDIR_ERR_TO_OUT:
            if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
                fprintf(stderr, "%s: dup2: %s\n", MSH_NAME, strerror(errno));
                return -1;
            }
            break;
        case REDIR_BOTH:
        case REDIR_BOTH_APPEND:
            fd = open_redirection(redirection->target,
                                  O_WRONLY | O_CREAT |
                                  (redirection->type == REDIR_BOTH ? O_TRUNC
                                                                  : O_APPEND));
            if (fd < 0) return -1;
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
                fprintf(stderr, "%s: dup2: %s\n", MSH_NAME, strerror(errno));
                close(fd);
                return -1;
            }
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
            break;
        }
    }
    return 0;
}

static int execute_parent_builtin(Shell *shell, ExpandedCommand *command,
                                  BuiltinKind builtin)
{
    int saved[3] = { -1, -1, -1 };
    (void)fflush(NULL);
    for (int fd = 0; fd < 3; ++fd) {
        saved[fd] = dup(fd);
        if (saved[fd] < 0) {
            fprintf(stderr, "%s: dup: %s\n", MSH_NAME, strerror(errno));
            for (int previous = 0; previous < fd; ++previous) close(saved[previous]);
            return 1;
        }
    }

    int status;
    if (apply_redirections(command) < 0) {
        status = 1;
    } else if (command->argc == 0) {
        status = 0;
    } else {
        status = builtin_run(shell, builtin, command->argc, command->argv);
    }
    (void)fflush(NULL);

    for (int fd = 0; fd < 3; ++fd) {
        if (dup2(saved[fd], fd) < 0) {
            fprintf(stderr, "%s: restoring fd %d: %s\n", MSH_NAME, fd,
                    strerror(errno));
            status = 1;
        }
        close(saved[fd]);
    }
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    return status;
}

static const char *redirection_spelling(RedirType type)
{
    switch (type) {
    case REDIR_IN: return "<";
    case REDIR_OUT: return ">";
    case REDIR_APPEND: return ">>";
    case REDIR_ERR: return "2>";
    case REDIR_ERR_APPEND: return "2>>";
    case REDIR_ERR_TO_OUT: return "2>&1";
    case REDIR_BOTH: return "&>";
    case REDIR_BOTH_APPEND: return "&>>";
    }
    return ">";
}

static char *pipeline_description(const ExpandedPipeline *pipeline)
{
    StrBuf description;
    strbuf_init(&description);
    for (size_t i = 0; i < pipeline->count; ++i) {
        if (i != 0) strbuf_append(&description, " | ");
        const ExpandedCommand *command = &pipeline->commands[i];
        if (command->argc == 0) strbuf_append_char(&description, ':');
        for (size_t j = 0; j < command->argc; ++j) {
            if (j != 0) strbuf_append_char(&description, ' ');
            strbuf_append(&description, command->argv[j]);
        }
        for (size_t j = 0; j < command->redir_count; ++j) {
            strbuf_append_char(&description, ' ');
            strbuf_append(&description,
                          redirection_spelling(command->redirs[j].type));
            if (command->redirs[j].target != NULL) {
                strbuf_append_char(&description, ' ');
                strbuf_append(&description, command->redirs[j].target);
            }
        }
    }
    return strbuf_take(&description);
}

static void close_pipes(int (*pipes)[2], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
    }
}

static int make_pipe(int descriptors[2])
{
    if (pipe(descriptors) < 0) {
        return -1;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(descriptors[i], F_GETFD);
        if (flags < 0 || fcntl(descriptors[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            int saved_errno = errno;
            close(descriptors[0]);
            close(descriptors[1]);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

static void reset_child_signals(void)
{
    const int signals[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU,
                            SIGCHLD };
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        (void)sigaction(signals[i], &action, NULL);
    }
}

static void child_wait_for_launch(int gate_read)
{
    unsigned char byte;
    for (;;) {
        ssize_t amount = read(gate_read, &byte, sizeof(byte));
        if (amount == 0) break;
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) break;
    }
    close(gate_read);
}

static void run_child(Shell *shell, const ExpandedPipeline *pipeline,
                      size_t index, int (*pipes)[2], size_t pipe_count,
                      int gate_read, int gate_write, pid_t pgid,
                      const sigset_t *old_mask)
{
    const ExpandedCommand *command = &pipeline->commands[index];
    pid_t child_pid = getpid();
    if (pgid == 0) pgid = child_pid;
    if (setpgid(0, pgid) < 0 && errno != EACCES) {
        _exit(126);
    }

    close(gate_write);
    child_wait_for_launch(gate_read);
    reset_child_signals();
    jobs_restore_sigmask(old_mask);

    if (index > 0 && dup2(pipes[index - 1][0], STDIN_FILENO) < 0) {
        _exit(126);
    }
    if (index + 1 < pipeline->count &&
        dup2(pipes[index][1], STDOUT_FILENO) < 0) {
        _exit(126);
    }
    close_pipes(pipes, pipe_count);
    if (apply_redirections(command) < 0) {
        _exit(1);
    }
    if (command->argc == 0) {
        _exit(0);
    }

    BuiltinKind builtin = builtin_identify(command->argv[0]);
    if (builtin != BUILTIN_NONE) {
        int status = builtin_run(shell, builtin, command->argc, command->argv);
        (void)fflush(NULL);
        _exit(status & 255);
    }

    execvp(command->argv[0], command->argv);
    int error = errno;
    if (error == ENOENT) {
        fprintf(stderr, "%s: %s: command not found\n", MSH_NAME,
                command->argv[0]);
        _exit(127);
    }
    fprintf(stderr, "%s: %s: %s\n", MSH_NAME, command->argv[0],
            strerror(error));
    _exit(error == EACCES || error == ENOEXEC ? 126 : 127);
}

static int launch_pipeline(Shell *shell, ExpandedPipeline *pipeline,
                           bool background, const char *source)
{
    size_t pipe_count = pipeline->count > 0 ? pipeline->count - 1 : 0;
    int (*pipes)[2] = xcalloc(pipe_count == 0 ? 1 : pipe_count, sizeof(*pipes));
    for (size_t i = 0; i < pipe_count; ++i) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
        if (make_pipe(pipes[i]) < 0) {
            fprintf(stderr, "%s: pipe: %s\n", MSH_NAME, strerror(errno));
            close_pipes(pipes, i);
            free(pipes);
            return 1;
        }
    }

    int gate[2] = { -1, -1 };
    if (make_pipe(gate) < 0) {
        fprintf(stderr, "%s: launch pipe: %s\n", MSH_NAME, strerror(errno));
        close_pipes(pipes, pipe_count);
        free(pipes);
        return 1;
    }

    (void)fflush(NULL);
    sigset_t old_mask;
    jobs_block_sigchld(&old_mask);
    Job *job = job_create(shell, source, background);
    pid_t pgid = 0;
    bool failed = false;

    for (size_t i = 0; i < pipeline->count; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            run_child(shell, pipeline, i, pipes, pipe_count, gate[0], gate[1],
                      pgid, &old_mask);
        }
        if (pid < 0) {
            fprintf(stderr, "%s: fork: %s\n", MSH_NAME, strerror(errno));
            failed = true;
            break;
        }
        if (pgid == 0) {
            pgid = pid;
            job->pgid = pgid;
        }
        if (setpgid(pid, pgid) < 0 && errno != EACCES && errno != ESRCH) {
            fprintf(stderr, "%s: setpgid: %s\n", MSH_NAME, strerror(errno));
            failed = true;
            (void)kill(pid, SIGKILL);
            break;
        }
        job_add_process(job, pid);
    }

    close(gate[0]);
    close_pipes(pipes, pipe_count);
    free(pipes);

    if (failed) {
        if (pgid > 0) (void)kill(-pgid, SIGKILL);
        close(gate[1]);
        jobs_restore_sigmask(&old_mask);
        jobs_process_events(shell);
        job_remove(shell, job);
        return 1;
    }

    if (!background && shell->interactive) {
        if (tcsetpgrp(shell->terminal_fd, pgid) < 0 && errno != ENOTTY) {
            fprintf(stderr, "%s: tcsetpgrp: %s\n", MSH_NAME, strerror(errno));
        }
    }
    close(gate[1]);
    jobs_restore_sigmask(&old_mask);

    if (background) {
        printf("[%d] %ld\n", job->id, (long)job->pgid);
        (void)fflush(stdout);
        return 0;
    }
    return job_wait_foreground(shell, job, false);
}

static int execute_pipeline(Shell *shell, const Pipeline *pipeline,
                            bool background, const char *source)
{
    (void)source;
    ExpandedPipeline expanded;
    if (expand_pipeline(shell, pipeline, &expanded) < 0) {
        return 1;
    }

    int status;
    ExpandedCommand *first = &expanded.commands[0];
    BuiltinKind builtin = first->argc == 0 ? BUILTIN_COLON
                                            : builtin_identify(first->argv[0]);
    if (expanded.count == 1 && !background &&
        (first->argc == 0 || builtin_requires_parent(builtin))) {
        status = execute_parent_builtin(shell, first, builtin);
    } else {
        char *description = pipeline_description(&expanded);
        status = launch_pipeline(shell, &expanded, background, description);
        free(description);
    }
    expanded_pipeline_free(&expanded);
    return status;
}

int execute_program(Shell *shell, const Program *program, const char *source)
{
    int status = shell->last_status;
    for (size_t list_index = 0; list_index < program->count; ++list_index) {
        const AndOr *and_or = &program->lists[list_index];
        if (and_or->background && and_or->count != 1) {
            fprintf(stderr,
                    "%s: background compound &&/|| lists are not supported\n",
                    MSH_NAME);
            status = 2;
            shell->last_status = status;
            continue;
        }

        for (size_t pipeline_index = 0; pipeline_index < and_or->count;
             ++pipeline_index) {
            if (pipeline_index > 0) {
                ConditionalOp op = and_or->operators[pipeline_index - 1];
                if ((op == COND_AND && status != 0) ||
                    (op == COND_OR && status == 0)) {
                    continue;
                }
            }
            status = execute_pipeline(shell, &and_or->pipelines[pipeline_index],
                                      and_or->background, source);
            shell->last_status = status;
            if (shell->should_exit) {
                return status;
            }
        }
    }
    return status;
}
