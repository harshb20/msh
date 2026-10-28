#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <mcheck.h>
#endif

static bool malloc_trace_start(void)
{
#ifdef __GLIBC__
    bool enabled = getenv("MALLOC_TRACE") != NULL;
    if (enabled) mtrace();
    return enabled;
#else
    return false;
#endif
}

static void malloc_trace_stop(bool enabled)
{
#ifdef __GLIBC__
    if (!enabled) return;
    (void)fflush(NULL);
    (void)fclose(stdin);
    (void)fclose(stdout);
    (void)fclose(stderr);
    muntrace();
#else
    (void)enabled;
#endif
}

static void install_ignored_signal(int signal_number)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(signal_number, &action, NULL) < 0) {
        perror(MSH_NAME ": sigaction");
        exit(1);
    }
}

static int shell_initialize(Shell *shell)
{
    memset(shell, 0, sizeof(*shell));
    shell->terminal_fd = STDIN_FILENO;
    shell->interactive = isatty(shell->terminal_fd) != 0;
    shell->shell_pgid = getpid();

    if (shell->interactive) {
        while (tcgetpgrp(shell->terminal_fd) != getpgrp()) {
            (void)kill(-getpgrp(), SIGTTIN);
        }
        install_ignored_signal(SIGINT);
        install_ignored_signal(SIGQUIT);
        install_ignored_signal(SIGTSTP);
        install_ignored_signal(SIGTTIN);
        install_ignored_signal(SIGTTOU);

        if (setpgid(shell->shell_pgid, shell->shell_pgid) < 0 &&
            errno != EACCES && errno != EPERM) {
            perror(MSH_NAME ": setpgid");
            return -1;
        }
        if (tcsetpgrp(shell->terminal_fd, shell->shell_pgid) < 0) {
            perror(MSH_NAME ": tcsetpgrp");
            return -1;
        }
        if (tcgetattr(shell->terminal_fd, &shell->shell_tmodes) < 0) {
            perror(MSH_NAME ": tcgetattr");
            return -1;
        }
    }

    if (jobs_initialize(shell) < 0) {
        return -1;
    }
    return 0;
}

static void shell_destroy(Shell *shell)
{
    jobs_shutdown(shell);
    for (size_t i = 0; i < shell->history_count; ++i) {
        free(shell->history[i]);
    }
    free(shell->history);
    free(shell->previous_directory);
}

static int process_line(Shell *shell, const char *line)
{
    TokenVec tokens;
    char *error_message = NULL;
    if (lex_line(line, &tokens, &error_message) < 0) {
        fprintf(stderr, "%s: syntax error: %s\n", MSH_NAME, error_message);
        free(error_message);
        shell->last_status = 2;
        return 2;
    }

    Program program;
    if (parse_tokens(&tokens, &program, &error_message) < 0) {
        fprintf(stderr, "%s: syntax error: %s\n", MSH_NAME, error_message);
        free(error_message);
        token_vec_free(&tokens);
        shell->last_status = 2;
        return 2;
    }
    token_vec_free(&tokens);

    int status = execute_program(shell, &program, line);
    program_free(&program);
    shell->last_status = status;
    return status;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "usage: %s [-c command]\n"
            "       %s --help | --version\n",
            MSH_NAME, MSH_NAME);
}

int main(int argc, char **argv)
{
    const char *command = NULL;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s %s\n", MSH_NAME, MSH_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        command = argv[2];
    } else if (argc != 1) {
        print_usage(stderr);
        return 2;
    }

    Shell shell;
    bool malloc_tracing = malloc_trace_start();
    if (shell_initialize(&shell) < 0) {
        malloc_trace_stop(malloc_tracing);
        return 1;
    }

    if (command != NULL) {
        (void)process_line(&shell, command);
    } else {
        const char *prompt = getenv("MSH_PROMPT");
        if (prompt == NULL) prompt = "msh$ ";
        while (!shell.should_exit) {
            jobs_notify(&shell);
            char *line = NULL;
            bool interrupted = false;
            if (!line_editor_read(&shell, prompt, &line, &interrupted)) {
                break;
            }
            if (interrupted) {
                shell.last_status = 130;
                free(line);
                continue;
            }
            history_add(&shell, line);
            (void)process_line(&shell, line);
            free(line);
        }
    }

    int status = shell.should_exit ? shell.exit_status : shell.last_status;
    shell_destroy(&shell);
    malloc_trace_stop(malloc_tracing);
    return status & 255;
}
