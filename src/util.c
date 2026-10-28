#include "shell.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t size)
{
    void *ptr = malloc(size == 0 ? 1 : size);
    if (ptr == NULL) {
        fprintf(stderr, "%s: out of memory\n", MSH_NAME);
        exit(2);
    }
    return ptr;
}

void *xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (ptr == NULL) {
        fprintf(stderr, "%s: out of memory\n", MSH_NAME);
        exit(2);
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size == 0 ? 1 : size);
    if (result == NULL) {
        fprintf(stderr, "%s: out of memory\n", MSH_NAME);
        exit(2);
    }
    return result;
}

char *xstrdup(const char *text)
{
    char *copy = strdup(text == NULL ? "" : text);
    if (copy == NULL) {
        fprintf(stderr, "%s: out of memory\n", MSH_NAME);
        exit(2);
    }
    return copy;
}

char *xstrndup(const char *text, size_t length)
{
    char *copy = xmalloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

void strbuf_init(StrBuf *buf)
{
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

static void strbuf_reserve(StrBuf *buf, size_t needed)
{
    if (needed <= buf->capacity) {
        return;
    }
    size_t capacity = buf->capacity == 0 ? 32 : buf->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            fprintf(stderr, "%s: string too large\n", MSH_NAME);
            exit(2);
        }
        capacity *= 2;
    }
    buf->data = xrealloc(buf->data, capacity);
    buf->capacity = capacity;
}

void strbuf_append_char(StrBuf *buf, char ch)
{
    strbuf_reserve(buf, buf->length + 2);
    buf->data[buf->length++] = ch;
    buf->data[buf->length] = '\0';
}

void strbuf_append_n(StrBuf *buf, const char *text, size_t length)
{
    strbuf_reserve(buf, buf->length + length + 1);
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
}

void strbuf_append(StrBuf *buf, const char *text)
{
    strbuf_append_n(buf, text, strlen(text));
}

char *strbuf_take(StrBuf *buf)
{
    if (buf->data == NULL) {
        return xstrdup("");
    }
    char *data = buf->data;
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
    return data;
}

void strbuf_free(StrBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

void string_vec_push(StringVec *vec, char *value)
{
    if (vec->count == vec->capacity) {
        size_t capacity = vec->capacity == 0 ? 8 : vec->capacity * 2;
        vec->items = xrealloc(vec->items, capacity * sizeof(*vec->items));
        vec->capacity = capacity;
    }
    vec->items[vec->count++] = value;
}

void string_vec_free(StringVec *vec)
{
    for (size_t i = 0; i < vec->count; ++i) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

char *join_argv(char *const *argv, size_t argc)
{
    StrBuf buf;
    strbuf_init(&buf);
    for (size_t i = 0; i < argc; ++i) {
        if (i != 0) {
            strbuf_append_char(&buf, ' ');
        }
        strbuf_append(&buf, argv[i]);
    }
    return strbuf_take(&buf);
}
