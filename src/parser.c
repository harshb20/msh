#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    TokenVec *tokens;
    size_t current;
    char **error_message;
} Parser;

static void command_free(Command *command);
static void pipeline_free(Pipeline *pipeline);

static void and_or_free(AndOr *and_or)
{
    for (size_t i = 0; i < and_or->count; ++i) {
        pipeline_free(&and_or->pipelines[i]);
    }
    free(and_or->pipelines);
    free(and_or->operators);
    memset(and_or, 0, sizeof(*and_or));
}

static Token *peek(Parser *parser)
{
    return &parser->tokens->items[parser->current];
}

static Token *advance(Parser *parser)
{
    return &parser->tokens->items[parser->current++];
}

static void parser_error(Parser *parser, const char *message)
{
    if (*parser->error_message != NULL) {
        return;
    }
    char buffer[256];
    (void)snprintf(buffer, sizeof(buffer), "%s near column %zu", message,
                   peek(parser)->position + 1);
    *parser->error_message = xstrdup(buffer);
}

static bool is_redirection(TokenType type)
{
    return type >= TOK_REDIR_IN && type <= TOK_REDIR_BOTH_APPEND;
}

static RedirType redirection_type(TokenType type)
{
    switch (type) {
    case TOK_REDIR_IN: return REDIR_IN;
    case TOK_REDIR_OUT: return REDIR_OUT;
    case TOK_REDIR_APPEND: return REDIR_APPEND;
    case TOK_REDIR_ERR: return REDIR_ERR;
    case TOK_REDIR_ERR_APPEND: return REDIR_ERR_APPEND;
    case TOK_REDIR_ERR_TO_OUT: return REDIR_ERR_TO_OUT;
    case TOK_REDIR_BOTH: return REDIR_BOTH;
    case TOK_REDIR_BOTH_APPEND: return REDIR_BOTH_APPEND;
    default: return REDIR_OUT;
    }
}

static void command_add_word(Command *command, Word *word)
{
    if (command->word_count == command->word_capacity) {
        size_t capacity = command->word_capacity == 0 ? 4
                                                       : command->word_capacity * 2;
        command->words = xrealloc(command->words,
                                  capacity * sizeof(*command->words));
        command->word_capacity = capacity;
    }
    command->words[command->word_count++] = word;
}

static void command_add_redirection(Command *command, Redirection redirection)
{
    if (command->redir_count == command->redir_capacity) {
        size_t capacity = command->redir_capacity == 0 ? 4
                                                        : command->redir_capacity * 2;
        command->redirs = xrealloc(command->redirs,
                                   capacity * sizeof(*command->redirs));
        command->redir_capacity = capacity;
    }
    command->redirs[command->redir_count++] = redirection;
}

static int parse_command(Parser *parser, Command *command)
{
    memset(command, 0, sizeof(*command));
    bool found_element = false;

    while (peek(parser)->type == TOK_WORD ||
           is_redirection(peek(parser)->type)) {
        found_element = true;
        if (peek(parser)->type == TOK_WORD) {
            Token *token = advance(parser);
            command_add_word(command, token->word);
            token->word = NULL;
            continue;
        }

        Redirection redirection;
        memset(&redirection, 0, sizeof(redirection));
        redirection.type = redirection_type(advance(parser)->type);
        if (redirection.type != REDIR_ERR_TO_OUT) {
            if (peek(parser)->type != TOK_WORD) {
                parser_error(parser, "expected a redirection target");
                command_free(command);
                return -1;
            }
            Token *target = advance(parser);
            redirection.target = target->word;
            target->word = NULL;
        }
        command_add_redirection(command, redirection);
    }

    if (!found_element) {
        parser_error(parser, "expected a command");
        return -1;
    }
    return 0;
}

static void pipeline_add_command(Pipeline *pipeline, Command command)
{
    if (pipeline->count == pipeline->capacity) {
        size_t capacity = pipeline->capacity == 0 ? 2 : pipeline->capacity * 2;
        pipeline->commands = xrealloc(pipeline->commands,
                                      capacity * sizeof(*pipeline->commands));
        pipeline->capacity = capacity;
    }
    pipeline->commands[pipeline->count++] = command;
}

static int parse_pipeline(Parser *parser, Pipeline *pipeline)
{
    memset(pipeline, 0, sizeof(*pipeline));
    Command command;
    if (parse_command(parser, &command) < 0) {
        return -1;
    }
    pipeline_add_command(pipeline, command);

    while (peek(parser)->type == TOK_PIPE) {
        advance(parser);
        if (parse_command(parser, &command) < 0) {
            pipeline_free(pipeline);
            return -1;
        }
        pipeline_add_command(pipeline, command);
    }
    return 0;
}

static void and_or_add_pipeline(AndOr *and_or, Pipeline pipeline,
                                bool has_operator, ConditionalOp op)
{
    if (and_or->count == and_or->capacity) {
        size_t capacity = and_or->capacity == 0 ? 2 : and_or->capacity * 2;
        and_or->pipelines = xrealloc(and_or->pipelines,
                                     capacity * sizeof(*and_or->pipelines));
        and_or->operators = xrealloc(and_or->operators,
                                     capacity * sizeof(*and_or->operators));
        and_or->capacity = capacity;
    }
    if (has_operator) {
        and_or->operators[and_or->count - 1] = op;
    }
    and_or->pipelines[and_or->count++] = pipeline;
}

static int parse_and_or(Parser *parser, AndOr *and_or)
{
    memset(and_or, 0, sizeof(*and_or));
    Pipeline pipeline;
    if (parse_pipeline(parser, &pipeline) < 0) {
        return -1;
    }
    and_or_add_pipeline(and_or, pipeline, false, COND_AND);

    while (peek(parser)->type == TOK_AND_IF ||
           peek(parser)->type == TOK_OR_IF) {
        ConditionalOp op = advance(parser)->type == TOK_AND_IF ? COND_AND
                                                               : COND_OR;
        if (parse_pipeline(parser, &pipeline) < 0) {
            and_or_free(and_or);
            return -1;
        }
        and_or_add_pipeline(and_or, pipeline, true, op);
    }
    return 0;
}

static void program_add_and_or(Program *program, AndOr and_or)
{
    if (program->count == program->capacity) {
        size_t capacity = program->capacity == 0 ? 4 : program->capacity * 2;
        program->lists = xrealloc(program->lists,
                                  capacity * sizeof(*program->lists));
        program->capacity = capacity;
    }
    program->lists[program->count++] = and_or;
}

int parse_tokens(TokenVec *tokens, Program *program, char **error_message)
{
    memset(program, 0, sizeof(*program));
    *error_message = NULL;
    Parser parser = { .tokens = tokens, .current = 0,
                      .error_message = error_message };

    if (peek(&parser)->type == TOK_END) {
        return 0;
    }

    while (peek(&parser)->type != TOK_END) {
        AndOr and_or;
        if (parse_and_or(&parser, &and_or) < 0) {
            program_free(program);
            return -1;
        }

        if (peek(&parser)->type == TOK_AMP) {
            and_or.background = true;
            advance(&parser);
        } else if (peek(&parser)->type == TOK_SEMI) {
            advance(&parser);
        } else if (peek(&parser)->type != TOK_END) {
            parser_error(&parser, "unexpected token");
            program_add_and_or(program, and_or);
            program_free(program);
            return -1;
        }

        program_add_and_or(program, and_or);
        if (peek(&parser)->type == TOK_END) {
            break;
        }
    }
    return 0;
}

static void command_free(Command *command)
{
    for (size_t i = 0; i < command->word_count; ++i) {
        word_free(command->words[i]);
    }
    for (size_t i = 0; i < command->redir_count; ++i) {
        word_free(command->redirs[i].target);
    }
    free(command->words);
    free(command->redirs);
}

static void pipeline_free(Pipeline *pipeline)
{
    for (size_t i = 0; i < pipeline->count; ++i) {
        command_free(&pipeline->commands[i]);
    }
    free(pipeline->commands);
}

void program_free(Program *program)
{
    for (size_t i = 0; i < program->count; ++i) {
        and_or_free(&program->lists[i]);
    }
    free(program->lists);
    program->lists = NULL;
    program->count = 0;
    program->capacity = 0;
}
