/*
 * Decodes PNG images into GEM monochrome forms for resgen by streaming
 * ImageMagick's PAM output: pixels are classified as ink, paper or
 * transparent from their RGBA values and packed into mask and data
 * word planes.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#define _POSIX_C_SOURCE 200809L
#include "resgen.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void free_image_form(image_form_t *form)
{
    if (form == NULL) {
        return;
    }
    free(form->mask_words);
    free(form->data_words);
    memset(form, 0, sizeof(*form));
}

static int spawn_convert_stream(const char *path, FILE **stream_out,
                                pid_t *pid_out)
{
    int pipe_fds[2];
    pid_t pid;

    if (path == NULL || stream_out == NULL || pid_out == NULL) {
        return 0;
    }
    if (pipe(pipe_fds) != 0) {
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 0;
    }
    if (pid == 0) {
        char *const argv_magick[] = {"magick", (char *)path, "-alpha", "set",
                                     "pam:-", NULL};
        char *const argv_convert[] = {"convert", (char *)path, "-alpha", "set",
                                      "pam:-", NULL};

        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        execvp("magick", argv_magick);
        execvp("convert", argv_convert);
        _exit(127);
    }

    close(pipe_fds[1]);
    *stream_out = fdopen(pipe_fds[0], "r");
    if (*stream_out == NULL) {
        close(pipe_fds[0]);
        (void)waitpid(pid, NULL, 0);
        return 0;
    }

    *pid_out = pid;
    return 1;
}

static int finish_convert_stream(FILE *stream, pid_t pid)
{
    int status = 0;

    if (stream != NULL) {
        fclose(stream);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int classify_pixel_rgba(unsigned int red, unsigned int green,
                               unsigned int blue, unsigned int alpha,
                               int *visible_out, int *black_out)
{
    unsigned int min_channel;

    if (visible_out == NULL || black_out == NULL) {
        return 0;
    }

    if (alpha < 32u) {
        *visible_out = 0;
        *black_out = 0;
        return 1;
    }
    if (alpha >= 224u && red == 0u && green == 0u && blue == 0u) {
        *visible_out = 1;
        *black_out = 1;
        return 1;
    }
    if (alpha >= 224u && red == 255u && green == 255u && blue == 255u) {
        *visible_out = 1;
        *black_out = 0;
        return 1;
    }

    min_channel = red;
    if (green < min_channel) {
        min_channel = green;
    }
    if (blue < min_channel) {
        min_channel = blue;
    }

    *visible_out = 1;
    *black_out = (min_channel < 224u);
    return 1;
}

static int alloc_form_words(image_form_t *form, WORD width, WORD height)
{
    size_t word_count;

    if (form == NULL || width <= 0 || height <= 0) {
        return 0;
    }

    form->width = width;
    form->height = height;
    form->words_per_row = (WORD)((width + 15) / 16);
    word_count = (size_t)form->words_per_row * (size_t)height;
    form->mask_words = calloc(word_count, sizeof(WORD));
    form->data_words = calloc(word_count, sizeof(WORD));
    if (form->mask_words == NULL || form->data_words == NULL) {
        free_image_form(form);
        return 0;
    }
    return 1;
}

static void set_form_pixel(image_form_t *form, WORD x, WORD y, int black)
{
    size_t index;
    WORD bit;

    if (form == NULL || x < 0 || y < 0 || x >= form->width ||
        y >= form->height) {
        return;
    }

    index = (size_t)y * (size_t)form->words_per_row + (size_t)x / 16u;
    bit = (WORD)((UWORD)0x8000u >> ((unsigned int)x & 15u));
    form->mask_words[index] |= bit;
    if (black) {
        form->data_words[index] |= bit;
    }
}

int load_png_form(const char *path, image_form_t *form)
{
    FILE *stream = NULL;
    pid_t pid = -1;
    char line[256];
    int width = 0;
    int height = 0;
    int depth = 0;
    int maxval = 0;
    int header_ok = 0;
    int sample_bytes;
    size_t pixel_bytes_len;
    unsigned char *pixel_buf;
    int x, y;

    if (path == NULL || form == NULL) {
        return 0;
    }
    if (!spawn_convert_stream(path, &stream, &pid)) {
        fprintf(stderr, "resgen: unable to start convert for %s\n", path);
        return 0;
    }

    while (fgets(line, sizeof(line), stream) != NULL) {
        if (strncmp(line, "ENDHDR", 6) == 0) {
            header_ok = 1;
            break;
        }
        (void)sscanf(line, "WIDTH %d", &width);
        (void)sscanf(line, "HEIGHT %d", &height);
        (void)sscanf(line, "DEPTH %d", &depth);
        (void)sscanf(line, "MAXVAL %d", &maxval);
    }

    if (!header_ok || width <= 0 || height <= 0 || depth <= 0 || maxval <= 0) {
        fprintf(stderr, "resgen: no pixel header from convert for %s\n", path);
        fclose(stream);
        (void)waitpid(pid, NULL, 0);
        return 0;
    }

    if (!alloc_form_words(form, (WORD)width, (WORD)height)) {
        fclose(stream);
        (void)waitpid(pid, NULL, 0);
        return 0;
    }

    sample_bytes = (maxval > 255) ? 2 : 1;
    pixel_bytes_len = (size_t)depth * (size_t)sample_bytes;
    pixel_buf = malloc(pixel_bytes_len);
    if (pixel_buf == NULL) {
        free_image_form(form);
        fclose(stream);
        (void)waitpid(pid, NULL, 0);
        return 0;
    }

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            unsigned int r = 0, g = 0, b = 0, a = 255;
            int visible = 0, black = 0;

            if (fread(pixel_buf, 1, pixel_bytes_len, stream) !=
                pixel_bytes_len) {
                fprintf(stderr,
                        "resgen: unexpected EOF reading pixels for %s\n",
                        path);
                free(pixel_buf);
                free_image_form(form);
                fclose(stream);
                (void)waitpid(pid, NULL, 0);
                return 0;
            }

            if (depth >= 4) {
                r = pixel_buf[0];
                g = pixel_buf[sample_bytes];
                b = pixel_buf[2 * sample_bytes];
                a = pixel_buf[3 * sample_bytes];
            } else if (depth == 3) {
                r = pixel_buf[0];
                g = pixel_buf[sample_bytes];
                b = pixel_buf[2 * sample_bytes];
                a = 255;
            } else if (depth == 2) {
                r = g = b = pixel_buf[0];
                a = pixel_buf[sample_bytes];
            } else {
                r = g = b = pixel_buf[0];
                a = 255;
            }

            if (!classify_pixel_rgba(r, g, b, a, &visible, &black)) {
                fprintf(stderr,
                        "resgen: unsupported pixel (%u,%u,%u,%u) in %s\n", r, g,
                        b, a, path);
                free(pixel_buf);
                free_image_form(form);
                fclose(stream);
                (void)waitpid(pid, NULL, 0);
                return 0;
            }

            if (visible) {
                set_form_pixel(form, (WORD)x, (WORD)y, black);
            }
        }
    }

    free(pixel_buf);

    if (!finish_convert_stream(stream, pid)) {
        fprintf(stderr, "resgen: convert failed for %s\n", path);
        free_image_form(form);
        return 0;
    }

    return 1;
}
