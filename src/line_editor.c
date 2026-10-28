#include "shell.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void write_all(int fd, const char *text, size_t length)
{
    while (length > 0) {
        ssize_t amount = write(fd, text, length);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            return;
        }
        text += (size_t)amount;
        length -= (size_t)amount;
    }
}

static void redraw(const char *prompt, const char *buffer, size_t length,
                   size_t cursor)
{
    write_all(STDOUT_FILENO, "\r\033[2K", 5);
    write_all(STDOUT_FILENO, prompt, strlen(prompt));
    write_all(STDOUT_FILENO, buffer, length);
    if (cursor < length) {
        char sequence[64];
        int count = snprintf(sequence, sizeof(sequence), "\033[%zuD",
                             length - cursor);
        if (count > 0) {
            write_all(STDOUT_FILENO, sequence, (size_t)count);
        }
    }
}

static int read_byte_timeout(unsigned char *byte, int timeout_ms)
{
    struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || !(descriptor.revents & POLLIN)) {
        return 0;
    }
    ssize_t amount = read(STDIN_FILENO, byte, 1);
    return amount == 1 ? 1 : 0;
}

static void replace_buffer(char **buffer, size_t *length, size_t *capacity,
                           size_t *cursor, const char *replacement)
{
    size_t needed = strlen(replacement) + 1;
    if (*capacity < needed) {
        *capacity = needed < 64 ? 64 : needed;
        *buffer = xrealloc(*buffer, *capacity);
    }
    memcpy(*buffer, replacement, needed);
    *length = needed - 1;
    *cursor = *length;
}

static void insert_character(char **buffer, size_t *length, size_t *capacity,
                             size_t *cursor, char ch)
{
    if (*length + 2 > *capacity) {
        *capacity = *capacity == 0 ? 64 : *capacity * 2;
        *buffer = xrealloc(*buffer, *capacity);
    }
    memmove(*buffer + *cursor + 1, *buffer + *cursor,
            *length - *cursor + 1);
    (*buffer)[*cursor] = ch;
    ++*cursor;
    ++*length;
}

static void delete_before_cursor(char *buffer, size_t *length, size_t *cursor)
{
    if (*cursor == 0) {
        return;
    }
    memmove(buffer + *cursor - 1, buffer + *cursor,
            *length - *cursor + 1);
    --*cursor;
    --*length;
}

static void delete_at_cursor(char *buffer, size_t *length, size_t cursor)
{
    if (cursor >= *length) {
        return;
    }
    memmove(buffer + cursor, buffer + cursor + 1, *length - cursor);
    --*length;
}

static void handle_escape(char *buffer, size_t *length, size_t *cursor,
                          Shell *shell, size_t *history_index, char **draft,
                          char **buffer_ptr, size_t *capacity)
{
    unsigned char first;
    unsigned char second;
    if (!read_byte_timeout(&first, 50) || first != '[' ||
        !read_byte_timeout(&second, 50)) {
        return;
    }
    switch (second) {
    case 'A':
        if (shell->history_count == 0 || *history_index == 0) break;
        if (*history_index == shell->history_count) {
            free(*draft);
            *draft = xstrdup(buffer);
        }
        --*history_index;
        replace_buffer(buffer_ptr, length, capacity, cursor,
                       shell->history[*history_index]);
        break;
    case 'B':
        if (*history_index >= shell->history_count) break;
        ++*history_index;
        replace_buffer(buffer_ptr, length, capacity, cursor,
                       *history_index == shell->history_count
                           ? (*draft == NULL ? "" : *draft)
                           : shell->history[*history_index]);
        break;
    case 'C':
        if (*cursor < *length) ++*cursor;
        break;
    case 'D':
        if (*cursor > 0) --*cursor;
        break;
    case 'H': *cursor = 0; break;
    case 'F': *cursor = *length; break;
    case '3': {
        unsigned char tilde;
        if (read_byte_timeout(&tilde, 50) && tilde == '~') {
            delete_at_cursor(*buffer_ptr, length, *cursor);
        }
        break;
    }
    default: break;
    }
}

void history_add(Shell *shell, const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (shell->history_count > 0 &&
        strcmp(shell->history[shell->history_count - 1], line) == 0) {
        return;
    }
    if (shell->history_count == shell->history_capacity) {
        size_t capacity = shell->history_capacity == 0 ? 64
                                                       : shell->history_capacity * 2;
        shell->history = xrealloc(shell->history,
                                  capacity * sizeof(*shell->history));
        shell->history_capacity = capacity;
    }
    shell->history[shell->history_count++] = xstrdup(line);
}

static int read_noninteractive(char **line_out)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t amount = getline(&line, &capacity, stdin);
    if (amount < 0) {
        free(line);
        return 0;
    }
    if (amount > 0 && line[amount - 1] == '\n') {
        line[amount - 1] = '\0';
    }
    *line_out = line;
    return 1;
}

int line_editor_read(Shell *shell, const char *prompt, char **line_out,
                     bool *interrupted)
{
    *line_out = NULL;
    *interrupted = false;
    if (!shell->interactive) {
        return read_noninteractive(line_out);
    }

    struct termios saved;
    if (tcgetattr(shell->terminal_fd, &saved) < 0) {
        return read_noninteractive(line_out);
    }
    struct termios raw = saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(shell->terminal_fd, TCSAFLUSH, &raw) < 0) {
        return read_noninteractive(line_out);
    }

    size_t capacity = 64;
    char *buffer = xcalloc(capacity, 1);
    size_t length = 0;
    size_t cursor = 0;
    size_t history_index = shell->history_count;
    char *draft = NULL;
    write_all(STDOUT_FILENO, prompt, strlen(prompt));

    int return_value = 1;
    bool finished = false;
    while (!finished) {
        struct pollfd descriptors[2];
        descriptors[0].fd = STDIN_FILENO;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = jobs_signal_fd();
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        int count = descriptors[1].fd >= 0 ? 2 : 1;

        int poll_result;
        do {
            poll_result = poll(descriptors, (nfds_t)count, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            return_value = 0;
            break;
        }

        if (count == 2 && (descriptors[1].revents & POLLIN)) {
            write_all(STDOUT_FILENO, "\r\033[2K", 5);
            jobs_notify(shell);
            redraw(prompt, buffer, length, cursor);
        }
        if (!(descriptors[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }

        unsigned char byte;
        ssize_t amount = read(STDIN_FILENO, &byte, 1);
        if (amount == 0) {
            return_value = 0;
            break;
        }
        if (amount < 0) {
            if (errno == EINTR) continue;
            return_value = 0;
            break;
        }

        switch (byte) {
        case '\r':
        case '\n':
            write_all(STDOUT_FILENO, "\r\n", 2);
            finished = true;
            break;
        case 3:
            write_all(STDOUT_FILENO, "^C\r\n", 4);
            *interrupted = true;
            buffer[0] = '\0';
            length = 0;
            finished = true;
            break;
        case 4:
            if (length == 0) {
                write_all(STDOUT_FILENO, "\r\n", 2);
                return_value = 0;
                finished = true;
            } else {
                delete_at_cursor(buffer, &length, cursor);
                redraw(prompt, buffer, length, cursor);
            }
            break;
        case 26:
            write_all(STDOUT_FILENO, "\a", 1);
            break;
        case 1: cursor = 0; redraw(prompt, buffer, length, cursor); break;
        case 5: cursor = length; redraw(prompt, buffer, length, cursor); break;
        case 11:
            buffer[cursor] = '\0';
            length = cursor;
            redraw(prompt, buffer, length, cursor);
            break;
        case 12:
            write_all(STDOUT_FILENO, "\033[2J\033[H", 7);
            redraw(prompt, buffer, length, cursor);
            break;
        case 21:
            memmove(buffer, buffer + cursor, length - cursor + 1);
            length -= cursor;
            cursor = 0;
            redraw(prompt, buffer, length, cursor);
            break;
        case 127:
        case 8:
            delete_before_cursor(buffer, &length, &cursor);
            redraw(prompt, buffer, length, cursor);
            break;
        case 27:
            handle_escape(buffer, &length, &cursor, shell, &history_index,
                          &draft, &buffer, &capacity);
            redraw(prompt, buffer, length, cursor);
            break;
        default:
            if (byte >= 32) {
                insert_character(&buffer, &length, &capacity, &cursor,
                                 (char)byte);
                redraw(prompt, buffer, length, cursor);
            }
            break;
        }
    }

    (void)tcsetattr(shell->terminal_fd, TCSAFLUSH, &saved);
    free(draft);
    if (return_value == 1) {
        buffer[length] = '\0';
        *line_out = buffer;
    } else {
        free(buffer);
    }
    return return_value;
}

