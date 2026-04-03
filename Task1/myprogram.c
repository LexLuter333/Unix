#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define DEFAULT_BLOCK_SIZE 4096

static int is_all_zeros(const char *buf, ssize_t len) {
    for (ssize_t i = 0; i < len; i++) {
        if (buf[i] != 0) return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    int opt;
    int block_size = DEFAULT_BLOCK_SIZE;
    int input_fd, output_fd;
    char *buffer = NULL;
    ssize_t bytes_read;
    off_t total_size = 0;
    char *input_filename = NULL;
    char *output_filename = NULL;

    while ((opt = getopt(argc, argv, "b:")) != -1) {
        if (opt == 'b') {
            block_size = atoi(optarg);
            if (block_size <= 0) {
                fprintf(stderr, "Ошибка: размер блока должен быть > 0\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Использование: %s [-b размер_блока] [вход] выход\n", argv[0]);
            return 1;
        }
    }

    int remaining = argc - optind;
    if (remaining == 1) {
        input_fd = STDIN_FILENO;
        output_filename = argv[optind];
    } else if (remaining == 2) {
        input_filename = argv[optind];
        output_filename = argv[optind + 1];
    } else {
        fprintf(stderr, "Использование:\n");
        fprintf(stderr, "  %s [-b N] output_file              # stdin -> output\n", argv[0]);
        fprintf(stderr, "  %s [-b N] input_file output_file   # input -> output\n", argv[0]);
        return 1;
    }

    if (input_filename) {
        input_fd = open(input_filename, O_RDONLY);
        if (input_fd < 0) {
            perror("Ошибка открытия входного файла");
            return 1;
        }
    }

    output_fd = open(output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        perror("Ошибка открытия выходного файла");
        if (input_filename) close(input_fd);
        return 1;
    }

    buffer = malloc(block_size);
    if (!buffer) {
        perror("Ошибка выделения памяти");
        if (input_filename) close(input_fd);
        close(output_fd);
        return 1;
    }

    while ((bytes_read = read(input_fd, buffer, block_size)) > 0) {
        if (is_all_zeros(buffer, bytes_read)) {
            if (lseek(output_fd, bytes_read, SEEK_CUR) == -1) {
                perror("Ошибка lseek");
                goto cleanup_error;
            }
        } else {
            if (write(output_fd, buffer, bytes_read) != bytes_read) {
                perror("Ошибка записи");
                goto cleanup_error;
            }
        }
        total_size += bytes_read;
    }

    if (bytes_read < 0) {
        perror("Ошибка чтения");
        goto cleanup_error;
    }

    if (ftruncate(output_fd, total_size) == -1) {
        perror("Ошибка ftruncate");
        goto cleanup_error;
    }

    struct stat st;
    if (fstat(output_fd, &st) == 0) {
        long logical = (long)total_size;
        long physical = (long)(st.st_blocks * 512);
        printf("Файл %s: логический=%ld байт, физический=%ld байт (экономия: %.2f%%)\n",
               output_filename, logical, physical,
               logical > 0 ? (100.0 * (logical - physical) / logical) : 0.0);
    }

    free(buffer);
    if (input_filename) close(input_fd);
    close(output_fd);
    return 0;

cleanup_error:
    free(buffer);
    if (input_filename) close(input_fd);
    close(output_fd);
    return 1;
}
