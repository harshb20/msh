#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

BuiltinKind builtin_identify(const char *name)
{
    if (name == NULL) return BUILTIN_NONE;
    if (strcmp(name, "cd") == 0) return BUILTIN_CD;
    if (strcmp(name, "pwd") == 0) return BUILTIN_PWD;
    if (strcmp(name, "exit") == 0) return BUILTIN_EXIT;
    if (strcmp(name, "export") == 0) return BUILTIN_EXPORT;
    if (strcmp(name, "unset") == 0) return BUILTIN_UNSET;
    if (strcmp(name, "echo") == 0) return BUILTIN_ECHO;
    if (strcmp(name, "jobs") == 0) return BUILTIN_JOBS;
    if (strcmp(name, "fg") == 0) return BUILTIN_FG;
    if (strcmp(name, "bg") == 0) return BUILTIN_BG;
    if (strcmp(name, "history") == 0) return BUILTIN_HISTORY;
    if (strcmp(name, ":") == 0) return BUILTIN_COLON;
    return BUILTIN_NONE;
}

bool builtin_requires_parent(BuiltinKind kind)
{
    return kind != BUILTIN_NONE;
}

static bool valid_identifier_n(const char *name, size_t length)
{
    if (length == 0 || !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    for (size_t i = 1; i < length; ++i) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

static int builtin_cd(Shell *shell, size_t argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "%s: cd: too many arguments\n", MSH_NAME);
        return 1;
    }

    const char *target;
    bool print_target = false;
    if (argc == 1) {
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "%s: cd: HOME not set\n", MSH_NAME);
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        target = shell->previous_directory;
        print_target = true;
        if (target == NULL) {
            fprintf(stderr, "%s: cd: OLDPWD not set\n", MSH_NAME);
            return 1;
        }
    } else {
        target = argv[1];
    }

    char *old_directory = getcwd(NULL, 0);
    if (chdir(target) < 0) {
        fprintf(stderr, "%s: cd: %s: %s\n", MSH_NAME, target,
                strerror(errno));
        free(old_directory);
        return 1;
    }

    if (old_directory != NULL) {
        free(shell->previous_directory);
        shell->previous_directory = old_directory;
        (void)setenv("OLDPWD", old_directory, 1);
    }
    char *current = getcwd(NULL, 0);
    if (current != NULL) {
        (void)setenv("PWD", current, 1);
        if (print_target) {
            puts(current);
        }
        free(current);
    }
    return 0;
}

static int builtin_pwd(size_t argc)
{
    if (argc > 1) {
        fprintf(stderr, "%s: pwd: arguments are not supported\n", MSH_NAME);
        return 2;
    }
    char *directory = getcwd(NULL, 0);
    if (directory == NULL) {
        fprintf(stderr, "%s: pwd: %s\n", MSH_NAME, strerror(errno));
        return 1;
    }
    puts(directory);
    free(directory);
    return 0;
}

static int builtin_exit(Shell *shell, size_t argc, char **argv)
{
    int status = shell->last_status;
    if (argc > 1) {
        char *end = NULL;
        errno = 0;
        long value = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0') {
            fprintf(stderr, "%s: exit: %s: numeric argument required\n",
                    MSH_NAME, argv[1]);
            shell->should_exit = true;
            shell->exit_status = 2;
            return 2;
        }
        status = (int)((unsigned long)value & 255UL);
    }
    if (argc > 2) {
        fprintf(stderr, "%s: exit: too many arguments\n", MSH_NAME);
        return 1;
    }
    shell->should_exit = true;
    shell->exit_status = status;
    return status;
}

static int builtin_export(size_t argc, char **argv)
{
    if (argc == 1) {
        fprintf(stderr, "%s: export: listing variables is not supported\n",
                MSH_NAME);
        return 2;
    }
    int result = 0;
    for (size_t i = 1; i < argc; ++i) {
        char *equals = strchr(argv[i], '=');
        size_t name_length = equals == NULL ? strlen(argv[i])
                                             : (size_t)(equals - argv[i]);
        if (!valid_identifier_n(argv[i], name_length)) {
            fprintf(stderr, "%s: export: `%s': not a valid identifier\n",
                    MSH_NAME, argv[i]);
            result = 1;
            continue;
        }
        char *name = xstrndup(argv[i], name_length);
        const char *value = equals == NULL ? getenv(name) : equals + 1;
        if (value == NULL) {
            value = "";
        }
        if (setenv(name, value, 1) < 0) {
            fprintf(stderr, "%s: export: %s\n", MSH_NAME, strerror(errno));
            result = 1;
        }
        free(name);
    }
    return result;
}

static int builtin_unset(size_t argc, char **argv)
{
    int result = 0;
    for (size_t i = 1; i < argc; ++i) {
        if (!valid_identifier_n(argv[i], strlen(argv[i]))) {
            fprintf(stderr, "%s: unset: `%s': not a valid identifier\n",
                    MSH_NAME, argv[i]);
            result = 1;
            continue;
        }
        if (unsetenv(argv[i]) < 0) {
            fprintf(stderr, "%s: unset: %s\n", MSH_NAME, strerror(errno));
            result = 1;
        }
    }
    return result;
}

static int builtin_echo(size_t argc, char **argv)
{
    bool newline = true;
    size_t first = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        first = 2;
    }
    for (size_t i = first; i < argc; ++i) {
        if (i != first) {
            putchar(' ');
        }
        fputs(argv[i], stdout);
    }
    if (newline) {
        putchar('\n');
    }
    return ferror(stdout) ? 1 : 0;
}

static int builtin_jobs(Shell *shell)
{
    jobs_process_events(shell);
    Job *job = shell->jobs;
    while (job != NULL) {
        Job *next = job->next;
        printf("[%d]%c  %-8s %s\n", job->id, job->next == NULL ? '+' : '-',
               job_state_name(job->state), job->command);
        job->notified = true;
        if (job->state == JOB_DONE) {
            job_remove(shell, job);
        }
        job = next;
    }
    return 0;
}

static int builtin_fg(Shell *shell, size_t argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "%s: fg: too many arguments\n", MSH_NAME);
        return 1;
    }
    jobs_process_events(shell);
    Job *job = job_find(shell, argc == 2 ? argv[1] : NULL);
    if (job == NULL) {
        fprintf(stderr, "%s: fg: no such job\n", MSH_NAME);
        return 1;
    }
    puts(job->command);
    return job_wait_foreground(shell, job, job->state == JOB_STOPPED);
}

static int builtin_bg(Shell *shell, size_t argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "%s: bg: too many arguments\n", MSH_NAME);
        return 1;
    }
    jobs_process_events(shell);
    Job *job = job_find(shell, argc == 2 ? argv[1] : NULL);
    if (job == NULL) {
        fprintf(stderr, "%s: bg: no such job\n", MSH_NAME);
        return 1;
    }
    if (job->state == JOB_RUNNING) {
        fprintf(stderr, "%s: bg: job is already running\n", MSH_NAME);
        return 1;
    }
    if (kill(-job->pgid, SIGCONT) < 0) {
        fprintf(stderr, "%s: bg: %s\n", MSH_NAME, strerror(errno));
        return 1;
    }
    for (Process *process = job->processes; process != NULL;
         process = process->next) {
        process->stopped = false;
    }
    job->state = JOB_RUNNING;
    job->background = true;
    job->notified = false;
    printf("[%d]+ %s &\n", job->id, job->command);
    return 0;
}

static int builtin_history(const Shell *shell)
{
    for (size_t i = 0; i < shell->history_count; ++i) {
        printf("%5zu  %s\n", i + 1, shell->history[i]);
    }
    return 0;
}

int builtin_run(Shell *shell, BuiltinKind kind, size_t argc, char **argv)
{
    switch (kind) {
    case BUILTIN_CD: return builtin_cd(shell, argc, argv);
    case BUILTIN_PWD: return builtin_pwd(argc);
    case BUILTIN_EXIT: return builtin_exit(shell, argc, argv);
    case BUILTIN_EXPORT: return builtin_export(argc, argv);
    case BUILTIN_UNSET: return builtin_unset(argc, argv);
    case BUILTIN_ECHO: return builtin_echo(argc, argv);
    case BUILTIN_JOBS: return builtin_jobs(shell);
    case BUILTIN_FG: return builtin_fg(shell, argc, argv);
    case BUILTIN_BG: return builtin_bg(shell, argc, argv);
    case BUILTIN_HISTORY: return builtin_history(shell);
    case BUILTIN_COLON: return 0;
    case BUILTIN_NONE: break;
    }
    return 127;
}
