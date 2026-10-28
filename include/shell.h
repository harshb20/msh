#ifndef MSH_SHELL_H
#define MSH_SHELL_H

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#define MSH_NAME "msh"
#define MSH_VERSION "1.0.0"

typedef enum {
    QUOTE_UNQUOTED,
    QUOTE_SINGLE,
    QUOTE_DOUBLE,
    QUOTE_LITERAL
} QuoteMode;

typedef struct {
    char *text;
    QuoteMode quote;
} WordPart;

typedef struct {
    WordPart *parts;
    size_t count;
    size_t capacity;
    bool had_quotes;
} Word;

typedef enum {
    TOK_WORD,
    TOK_PIPE,
    TOK_AND_IF,
    TOK_OR_IF,
    TOK_SEMI,
    TOK_AMP,
    TOK_REDIR_IN,
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
    TOK_REDIR_ERR,
    TOK_REDIR_ERR_APPEND,
    TOK_REDIR_ERR_TO_OUT,
    TOK_REDIR_BOTH,
    TOK_REDIR_BOTH_APPEND,
    TOK_END
} TokenType;

typedef struct {
    TokenType type;
    Word *word;
    size_t position;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenVec;

typedef enum {
    REDIR_IN,
    REDIR_OUT,
    REDIR_APPEND,
    REDIR_ERR,
    REDIR_ERR_APPEND,
    REDIR_ERR_TO_OUT,
    REDIR_BOTH,
    REDIR_BOTH_APPEND
} RedirType;

typedef struct {
    RedirType type;
    Word *target;
} Redirection;

typedef struct {
    Word **words;
    size_t word_count;
    size_t word_capacity;
    Redirection *redirs;
    size_t redir_count;
    size_t redir_capacity;
} Command;

typedef struct {
    Command *commands;
    size_t count;
    size_t capacity;
} Pipeline;

typedef enum {
    COND_AND,
    COND_OR
} ConditionalOp;

typedef struct {
    Pipeline *pipelines;
    ConditionalOp *operators;
    size_t count;
    size_t capacity;
    bool background;
} AndOr;

typedef struct {
    AndOr *lists;
    size_t count;
    size_t capacity;
} Program;

typedef struct Process Process;
typedef struct Job Job;

struct Process {
    pid_t pid;
    bool completed;
    bool stopped;
    int status;
    Process *next;
};

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

struct Job {
    int id;
    pid_t pgid;
    char *command;
    Process *processes;
    pid_t last_pid;
    JobState state;
    bool background;
    bool notified;
    struct termios tmodes;
    Job *next;
};

typedef struct {
    bool interactive;
    int terminal_fd;
    pid_t shell_pgid;
    struct termios shell_tmodes;
    int last_status;
    int exit_status;
    bool should_exit;
    char *previous_directory;
    Job *jobs;
    int next_job_id;
    char **history;
    size_t history_count;
    size_t history_capacity;
} Shell;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringVec;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StrBuf;

typedef enum {
    BUILTIN_NONE,
    BUILTIN_CD,
    BUILTIN_PWD,
    BUILTIN_EXIT,
    BUILTIN_EXPORT,
    BUILTIN_UNSET,
    BUILTIN_ECHO,
    BUILTIN_JOBS,
    BUILTIN_FG,
    BUILTIN_BG,
    BUILTIN_HISTORY,
    BUILTIN_COLON
} BuiltinKind;

/* util.c */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *text);
char *xstrndup(const char *text, size_t length);
void strbuf_init(StrBuf *buf);
void strbuf_append_char(StrBuf *buf, char ch);
void strbuf_append(StrBuf *buf, const char *text);
void strbuf_append_n(StrBuf *buf, const char *text, size_t length);
char *strbuf_take(StrBuf *buf);
void strbuf_free(StrBuf *buf);
void string_vec_push(StringVec *vec, char *value);
void string_vec_free(StringVec *vec);
char *join_argv(char *const *argv, size_t argc);

/* lexer.c */
int lex_line(const char *line, TokenVec *tokens, char **error_message);
void token_vec_free(TokenVec *tokens);
void word_free(Word *word);

/* parser.c */
int parse_tokens(TokenVec *tokens, Program *program, char **error_message);
void program_free(Program *program);

/* expand.c */
int expand_word(const Shell *shell, const Word *word, StringVec *result,
                char **error_message);

/* jobs.c */
int jobs_initialize(Shell *shell);
void jobs_shutdown(Shell *shell);
void jobs_block_sigchld(sigset_t *old_mask);
void jobs_restore_sigmask(const sigset_t *old_mask);
int jobs_signal_fd(void);
void jobs_process_events(Shell *shell);
void jobs_notify(Shell *shell);
Job *job_create(Shell *shell, const char *command, bool background);
void job_add_process(Job *job, pid_t pid);
void job_remove(Shell *shell, Job *job);
Job *job_find(Shell *shell, const char *spec);
int job_wait_foreground(Shell *shell, Job *job, bool send_continue);
const char *job_state_name(JobState state);
int job_last_status(const Job *job);

/* builtins.c */
BuiltinKind builtin_identify(const char *name);
bool builtin_requires_parent(BuiltinKind kind);
int builtin_run(Shell *shell, BuiltinKind kind, size_t argc, char **argv);

/* exec.c */
int execute_program(Shell *shell, const Program *program, const char *source);

/* line_editor.c */
int line_editor_read(Shell *shell, const char *prompt, char **line_out,
                     bool *interrupted);
void history_add(Shell *shell, const char *line);

#endif
