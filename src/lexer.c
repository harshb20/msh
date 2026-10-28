#include "shell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Word *word_create(void)
{
    return xcalloc(1, sizeof(Word));
}

static void word_add_part(Word *word, QuoteMode quote, const char *text,
                          size_t length)
{
    if (quote != QUOTE_UNQUOTED) {
        word->had_quotes = true;
    }

    if (word->count > 0 && word->parts[word->count - 1].quote == quote) {
        WordPart *part = &word->parts[word->count - 1];
        size_t old_length = strlen(part->text);
        part->text = xrealloc(part->text, old_length + length + 1);
        memcpy(part->text + old_length, text, length);
        part->text[old_length + length] = '\0';
        return;
    }

    if (word->count == word->capacity) {
        size_t capacity = word->capacity == 0 ? 4 : word->capacity * 2;
        word->parts = xrealloc(word->parts,
                               capacity * sizeof(*word->parts));
        word->capacity = capacity;
    }
    word->parts[word->count].text = xstrndup(text, length);
    word->parts[word->count].quote = quote;
    ++word->count;
}

void word_free(Word *word)
{
    if (word == NULL) {
        return;
    }
    for (size_t i = 0; i < word->count; ++i) {
        free(word->parts[i].text);
    }
    free(word->parts);
    free(word);
}

static void token_push(TokenVec *tokens, TokenType type, Word *word,
                       size_t position)
{
    if (tokens->count == tokens->capacity) {
        size_t capacity = tokens->capacity == 0 ? 16 : tokens->capacity * 2;
        tokens->items = xrealloc(tokens->items,
                                 capacity * sizeof(*tokens->items));
        tokens->capacity = capacity;
    }
    tokens->items[tokens->count].type = type;
    tokens->items[tokens->count].word = word;
    tokens->items[tokens->count].position = position;
    ++tokens->count;
}

static bool operator_start(char ch)
{
    return ch == '|' || ch == '&' || ch == ';' || ch == '<' || ch == '>';
}

static bool match_operator(const char *line, size_t *index, TokenType *type)
{
    const char *text = line + *index;
    size_t amount = 0;

    if (strncmp(text, "2>&1", 4) == 0) {
        *type = TOK_REDIR_ERR_TO_OUT;
        amount = 4;
    } else if (strncmp(text, "2>>", 3) == 0) {
        *type = TOK_REDIR_ERR_APPEND;
        amount = 3;
    } else if (strncmp(text, "&>>", 3) == 0) {
        *type = TOK_REDIR_BOTH_APPEND;
        amount = 3;
    } else if (strncmp(text, "2>", 2) == 0) {
        *type = TOK_REDIR_ERR;
        amount = 2;
    } else if (strncmp(text, "1>>", 3) == 0) {
        *type = TOK_REDIR_APPEND;
        amount = 3;
    } else if (strncmp(text, "1>", 2) == 0) {
        *type = TOK_REDIR_OUT;
        amount = 2;
    } else if (strncmp(text, "0<", 2) == 0) {
        *type = TOK_REDIR_IN;
        amount = 2;
    } else if (strncmp(text, "&>", 2) == 0) {
        *type = TOK_REDIR_BOTH;
        amount = 2;
    } else if (strncmp(text, ">>", 2) == 0) {
        *type = TOK_REDIR_APPEND;
        amount = 2;
    } else if (strncmp(text, "&&", 2) == 0) {
        *type = TOK_AND_IF;
        amount = 2;
    } else if (strncmp(text, "||", 2) == 0) {
        *type = TOK_OR_IF;
        amount = 2;
    } else {
        switch (*text) {
        case '|': *type = TOK_PIPE; amount = 1; break;
        case '&': *type = TOK_AMP; amount = 1; break;
        case ';': *type = TOK_SEMI; amount = 1; break;
        case '<': *type = TOK_REDIR_IN; amount = 1; break;
        case '>': *type = TOK_REDIR_OUT; amount = 1; break;
        default: return false;
        }
    }

    *index += amount;
    return true;
}

static int lex_word(const char *line, size_t *index, Word **word_out,
                    char **error_message)
{
    Word *word = word_create();

    while (line[*index] != '\0') {
        char ch = line[*index];
        if (isspace((unsigned char)ch) || operator_start(ch)) {
            break;
        }

        if (ch == '\\') {
            ++*index;
            if (line[*index] == '\0') {
                *error_message = xstrdup("trailing backslash");
                word_free(word);
                return -1;
            }
            word_add_part(word, QUOTE_LITERAL, line + *index, 1);
            ++*index;
            continue;
        }

        if (ch == '\'') {
            ++*index;
            word_add_part(word, QUOTE_SINGLE, "", 0);
            size_t start = *index;
            while (line[*index] != '\0' && line[*index] != '\'') {
                ++*index;
            }
            if (line[*index] != '\'') {
                *error_message = xstrdup("unterminated single quote");
                word_free(word);
                return -1;
            }
            word_add_part(word, QUOTE_SINGLE, line + start, *index - start);
            ++*index;
            continue;
        }

        if (ch == '"') {
            ++*index;
            word_add_part(word, QUOTE_DOUBLE, "", 0);
            while (line[*index] != '\0' && line[*index] != '"') {
                if (line[*index] == '\\') {
                    char next = line[*index + 1];
                    if (next == '$' || next == '"' || next == '\\' ||
                        next == '\n') {
                        if (next == '\n') {
                            *index += 2;
                            continue;
                        }
                        word_add_part(word, QUOTE_LITERAL,
                                      line + *index + 1, 1);
                        *index += 2;
                        continue;
                    }
                    word_add_part(word, QUOTE_DOUBLE, line + *index, 1);
                    ++*index;
                    continue;
                }
                word_add_part(word, QUOTE_DOUBLE, line + *index, 1);
                ++*index;
            }
            if (line[*index] != '"') {
                *error_message = xstrdup("unterminated double quote");
                word_free(word);
                return -1;
            }
            ++*index;
            continue;
        }

        size_t start = *index;
        while (line[*index] != '\0' &&
               !isspace((unsigned char)line[*index]) &&
               !operator_start(line[*index]) && line[*index] != '\\' &&
               line[*index] != '\'' && line[*index] != '"') {
            ++*index;
        }
        word_add_part(word, QUOTE_UNQUOTED, line + start, *index - start);
    }

    if (word->count == 0) {
        word_free(word);
        *error_message = xstrdup("empty word");
        return -1;
    }
    *word_out = word;
    return 0;
}

int lex_line(const char *line, TokenVec *tokens, char **error_message)
{
    memset(tokens, 0, sizeof(*tokens));
    *error_message = NULL;

    size_t index = 0;
    while (line[index] != '\0') {
        while (isspace((unsigned char)line[index])) {
            ++index;
        }
        if (line[index] == '\0' || line[index] == '#') {
            break;
        }

        size_t position = index;
        TokenType type;
        if (match_operator(line, &index, &type)) {
            token_push(tokens, type, NULL, position);
            continue;
        }

        Word *word = NULL;
        if (lex_word(line, &index, &word, error_message) < 0) {
            token_vec_free(tokens);
            return -1;
        }
        token_push(tokens, TOK_WORD, word, position);
    }

    token_push(tokens, TOK_END, NULL, index);
    return 0;
}

void token_vec_free(TokenVec *tokens)
{
    for (size_t i = 0; i < tokens->count; ++i) {
        word_free(tokens->items[i].word);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
    tokens->capacity = 0;
}

