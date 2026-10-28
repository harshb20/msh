#include "shell.h"

#include <ctype.h>
#include <glob.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *text;
    bool *quoted;
    size_t length;
    size_t capacity;
    bool force_field;
} ExpandedText;

typedef struct {
    char *text;
    bool *quoted;
    size_t length;
} Field;

typedef struct {
    Field *items;
    size_t count;
    size_t capacity;
} FieldVec;

static void expanded_reserve(ExpandedText *text, size_t needed)
{
    if (needed <= text->capacity) {
        return;
    }
    size_t capacity = text->capacity == 0 ? 32 : text->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    text->text = xrealloc(text->text, capacity);
    text->quoted = xrealloc(text->quoted, capacity * sizeof(*text->quoted));
    text->capacity = capacity;
}

static void expanded_append_n(ExpandedText *text, const char *value,
                              size_t length, bool quoted)
{
    expanded_reserve(text, text->length + length + 1);
    memcpy(text->text + text->length, value, length);
    for (size_t i = 0; i < length; ++i) {
        text->quoted[text->length + i] = quoted;
    }
    text->length += length;
    text->text[text->length] = '\0';
}

static void expanded_append(ExpandedText *text, const char *value, bool quoted)
{
    expanded_append_n(text, value, strlen(value), quoted);
}

static bool identifier_start(char ch)
{
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool identifier_char(char ch)
{
    return isalnum((unsigned char)ch) || ch == '_';
}

static int append_parameter(const Shell *shell, const char *source,
                            size_t *index, bool quoted, ExpandedText *output,
                            char **error_message)
{
    size_t cursor = *index;
    if (source[cursor] != '$') {
        return 0;
    }
    ++cursor;

    if (source[cursor] == '?') {
        char number[32];
        (void)snprintf(number, sizeof(number), "%d", shell->last_status);
        expanded_append(output, number, quoted);
        *index = cursor + 1;
        return 1;
    }
    if (source[cursor] == '$') {
        char number[32];
        (void)snprintf(number, sizeof(number), "%ld", (long)shell->shell_pgid);
        expanded_append(output, number, quoted);
        *index = cursor + 1;
        return 1;
    }

    size_t start;
    size_t length;
    if (source[cursor] == '{') {
        ++cursor;
        start = cursor;
        while (source[cursor] != '\0' && source[cursor] != '}') {
            ++cursor;
        }
        if (source[cursor] != '}') {
            *error_message = xstrdup("bad substitution: missing '}'");
            return -1;
        }
        length = cursor - start;
        if (length == 0 || !identifier_start(source[start])) {
            *error_message = xstrdup("bad substitution");
            return -1;
        }
        for (size_t i = 1; i < length; ++i) {
            if (!identifier_char(source[start + i])) {
                *error_message = xstrdup("bad substitution");
                return -1;
            }
        }
        *index = cursor + 1;
    } else if (identifier_start(source[cursor])) {
        start = cursor;
        while (identifier_char(source[cursor])) {
            ++cursor;
        }
        length = cursor - start;
        *index = cursor;
    } else if (isdigit((unsigned char)source[cursor])) {
        *index = cursor + 1;
        return 1;
    } else {
        expanded_append_n(output, "$", 1, quoted);
        *index = cursor;
        return 1;
    }

    char *name = xstrndup(source + start, length);
    const char *value = getenv(name);
    if (value != NULL) {
        expanded_append(output, value, quoted);
    }
    free(name);
    return 1;
}

static size_t tilde_prefix_length(const char *text)
{
    if (text[0] != '~') {
        return 0;
    }
    size_t length = 1;
    while (text[length] != '\0' && text[length] != '/') {
        ++length;
    }
    return length;
}

static const char *tilde_home(const char *text, size_t prefix_length,
                              char **allocated_user)
{
    *allocated_user = NULL;
    if (prefix_length == 1) {
        const char *home = getenv("HOME");
        if (home != NULL) {
            return home;
        }
        struct passwd *entry = getpwuid(getuid());
        return entry == NULL ? NULL : entry->pw_dir;
    }

    *allocated_user = xstrndup(text + 1, prefix_length - 1);
    struct passwd *entry = getpwnam(*allocated_user);
    return entry == NULL ? NULL : entry->pw_dir;
}

static int expand_parts(const Shell *shell, const Word *word,
                        ExpandedText *output, char **error_message)
{
    memset(output, 0, sizeof(*output));
    output->force_field = word->had_quotes;

    for (size_t part_index = 0; part_index < word->count; ++part_index) {
        const WordPart *part = &word->parts[part_index];
        const char *text = part->text;
        bool quoted = part->quote != QUOTE_UNQUOTED;
        bool expandable = part->quote == QUOTE_UNQUOTED ||
                          part->quote == QUOTE_DOUBLE;

        size_t index = 0;
        if (part_index == 0 && part->quote == QUOTE_UNQUOTED && text[0] == '~') {
            size_t prefix = tilde_prefix_length(text);
            char *user = NULL;
            const char *home = tilde_home(text, prefix, &user);
            if (home != NULL) {
                expanded_append(output, home, true);
                index = prefix;
            }
            free(user);
        }

        while (text[index] != '\0') {
            if (expandable && text[index] == '$') {
                int result = append_parameter(shell, text, &index, quoted,
                                              output, error_message);
                if (result < 0) {
                    free(output->text);
                    free(output->quoted);
                    memset(output, 0, sizeof(*output));
                    return -1;
                }
                if (result > 0) {
                    continue;
                }
            }
            expanded_append_n(output, text + index, 1, quoted);
            ++index;
        }
    }
    return 0;
}

static void field_vec_push(FieldVec *fields, const ExpandedText *expanded,
                           size_t start, size_t end)
{
    if (fields->count == fields->capacity) {
        size_t capacity = fields->capacity == 0 ? 4 : fields->capacity * 2;
        fields->items = xrealloc(fields->items,
                                 capacity * sizeof(*fields->items));
        fields->capacity = capacity;
    }
    Field *field = &fields->items[fields->count++];
    field->length = end - start;
    field->text = xstrndup(expanded->text == NULL ? "" : expanded->text + start,
                           field->length);
    field->quoted = xcalloc(field->length == 0 ? 1 : field->length,
                            sizeof(*field->quoted));
    if (field->length > 0) {
        memcpy(field->quoted, expanded->quoted + start,
               field->length * sizeof(*field->quoted));
    }
}

static void split_fields(const ExpandedText *expanded, FieldVec *fields)
{
    memset(fields, 0, sizeof(*fields));
    size_t index = 0;
    while (index < expanded->length) {
        while (index < expanded->length && !expanded->quoted[index] &&
               (expanded->text[index] == ' ' || expanded->text[index] == '\t' ||
                expanded->text[index] == '\n')) {
            ++index;
        }
        if (index == expanded->length) {
            break;
        }
        size_t start = index;
        while (index < expanded->length &&
               (expanded->quoted[index] ||
                (expanded->text[index] != ' ' &&
                 expanded->text[index] != '\t' &&
                 expanded->text[index] != '\n'))) {
            ++index;
        }
        field_vec_push(fields, expanded, start, index);
    }

    if (fields->count == 0 && expanded->force_field) {
        field_vec_push(fields, expanded, 0, 0);
    }
}

static bool field_has_glob(const Field *field)
{
    for (size_t i = 0; i < field->length; ++i) {
        if (!field->quoted[i] && (field->text[i] == '*' ||
                                  field->text[i] == '?' ||
                                  field->text[i] == '[')) {
            return true;
        }
    }
    return false;
}

static char *field_glob_pattern(const Field *field)
{
    StrBuf pattern;
    strbuf_init(&pattern);
    for (size_t i = 0; i < field->length; ++i) {
        char ch = field->text[i];
        if (field->quoted[i] && (ch == '*' || ch == '?' || ch == '[' ||
                                 ch == ']' || ch == '\\')) {
            strbuf_append_char(&pattern, '\\');
        }
        strbuf_append_char(&pattern, ch);
    }
    return strbuf_take(&pattern);
}

static int glob_field(const Field *field, StringVec *result,
                      char **error_message)
{
    if (!field_has_glob(field)) {
        string_vec_push(result, xstrdup(field->text));
        return 0;
    }

    char *pattern = field_glob_pattern(field);
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    int status = glob(pattern, 0, NULL, &matches);
    free(pattern);

    if (status == GLOB_NOMATCH) {
        string_vec_push(result, xstrdup(field->text));
        globfree(&matches);
        return 0;
    }
    if (status != 0) {
        *error_message = xstrdup("pathname expansion failed");
        globfree(&matches);
        return -1;
    }
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
        string_vec_push(result, xstrdup(matches.gl_pathv[i]));
    }
    globfree(&matches);
    return 0;
}

int expand_word(const Shell *shell, const Word *word, StringVec *result,
                char **error_message)
{
    *error_message = NULL;
    ExpandedText expanded;
    if (expand_parts(shell, word, &expanded, error_message) < 0) {
        return -1;
    }

    FieldVec fields;
    split_fields(&expanded, &fields);
    free(expanded.text);
    free(expanded.quoted);

    int status = 0;
    for (size_t i = 0; i < fields.count; ++i) {
        if (glob_field(&fields.items[i], result, error_message) < 0) {
            status = -1;
            break;
        }
    }

    for (size_t i = 0; i < fields.count; ++i) {
        free(fields.items[i].text);
        free(fields.items[i].quoted);
    }
    free(fields.items);
    return status;
}
