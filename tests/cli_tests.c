#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include "windows_test_process.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  int64_t size;
  time_t seconds;
  long nanoseconds;
  uint64_t hash;
} file_snapshot;

typedef struct {
  char directory[PATH_MAX];
  char stdout_path[PATH_MAX];
  char stderr_path[PATH_MAX];
} test_workspace;

static const char *g_analyzer = NULL;
static int g_failures = 0;
static int g_skipped = 0;

#define CHECK(condition, ...)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                    \
      fprintf(stderr, __VA_ARGS__);                                            \
      fputc('\n', stderr);                                                     \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

static int join_path(char *destination,
                     size_t capacity,
                     const char *directory,
                     const char *name) {
  const int count = snprintf(destination, capacity, "%s/%s", directory, name);
  return count >= 0 && (size_t)count < capacity;
}

static long process_id(void) {
#if defined(_WIN32)
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

static int make_directory(const char *path) {
#if defined(_WIN32)
  return _mkdir(path);
#else
  return mkdir(path, 0700);
#endif
}

static int remove_file(const char *path) {
#if defined(_WIN32)
  return _unlink(path);
#else
  return unlink(path);
#endif
}

static int remove_directory(const char *path) {
#if defined(_WIN32)
  return _rmdir(path);
#else
  return rmdir(path);
#endif
}

static int make_file_read_only(const char *path) {
#if defined(_WIN32)
  return _chmod(path, _S_IREAD);
#else
  return chmod(path, 0444);
#endif
}

static int make_file_writable(const char *path) {
#if defined(_WIN32)
  return _chmod(path, _S_IREAD | _S_IWRITE);
#else
  return chmod(path, 0644);
#endif
}

static int workspace_open(test_workspace *workspace) {
  unsigned int attempt;
  int made_directory = 0;
#if defined(_WIN32)
  const char *temporary_root = getenv("TEMP");
  if (temporary_root == NULL || temporary_root[0] == '\0') {
    temporary_root = ".";
  }
#else
  const char *temporary_root = "/tmp";
#endif

  memset(workspace, 0, sizeof(*workspace));
  for (attempt = 0u; attempt < 100u; attempt++) {
    const int count = snprintf(workspace->directory,
                               sizeof(workspace->directory),
                               "%s/hlolli-wg-analyzer-test-%ld-%u",
                               temporary_root,
                               process_id(),
                               attempt);
    if (count < 0 || (size_t)count >= sizeof(workspace->directory)) {
      return 0;
    }
    if (make_directory(workspace->directory) == 0) {
      made_directory = 1;
      break;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  if (!made_directory) {
    fprintf(stderr, "temporary directory creation failed: %s\n", strerror(errno));
    return 0;
  }
  if (!join_path(workspace->stdout_path,
                 sizeof(workspace->stdout_path),
                 workspace->directory,
                 "stdout.txt") ||
      !join_path(workspace->stderr_path,
                 sizeof(workspace->stderr_path),
                 workspace->directory,
                 "stderr.txt")) {
    fprintf(stderr, "temporary path is too long\n");
    return 0;
  }
  return 1;
}

static void workspace_close(test_workspace *workspace) {
  (void)remove_file(workspace->stdout_path);
  (void)remove_file(workspace->stderr_path);
  (void)remove_directory(workspace->directory);
}

static int write_bytes(FILE *stream, const void *data, size_t size) {
  return fwrite(data, 1, size, stream) == size;
}

static int write_u16le(FILE *stream, uint16_t value) {
  const unsigned char bytes[2] = {(unsigned char)(value & 0xffu),
                                  (unsigned char)((value >> 8u) & 0xffu)};
  return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_u32le(FILE *stream, uint32_t value) {
  const unsigned char bytes[4] = {
      (unsigned char)(value & 0xffu),
      (unsigned char)((value >> 8u) & 0xffu),
      (unsigned char)((value >> 16u) & 0xffu),
      (unsigned char)((value >> 24u) & 0xffu)};
  return write_bytes(stream, bytes, sizeof(bytes));
}

static int16_t fixture_sample(uint32_t frame, uint16_t channel) {
  static const int16_t cycle[8] = {
      0, 8192, 16384, 8192, 0, -8192, -16384, -8192};
  const int16_t value = cycle[frame % 8u];
  return channel == 0u ? value : (int16_t)(-value / 2);
}

static uint32_t fixture_float_bits(int16_t sample) {
  uint32_t bits;
  const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
  if (magnitude == 0) {
    bits = 0u;
  } else if (magnitude == 4096) {
    bits = UINT32_C(0x3e000000);
  } else if (magnitude == 8192) {
    bits = UINT32_C(0x3e800000);
  } else {
    bits = UINT32_C(0x3f000000);
  }
  if (sample < 0) {
    bits |= UINT32_C(0x80000000);
  }
  return bits;
}

static int write_wave(const char *path,
                      uint16_t encoding,
                      uint16_t channels,
                      uint32_t sample_rate,
                      uint16_t bits_per_sample,
                      uint32_t declared_frames,
                      uint32_t written_frames) {
  const uint16_t bytes_per_sample = (uint16_t)(bits_per_sample / 8u);
  const uint16_t block_align = (uint16_t)(channels * bytes_per_sample);
  const uint32_t byte_rate = sample_rate * (uint32_t)block_align;
  const uint32_t data_bytes = declared_frames * (uint32_t)block_align;
  const uint32_t written_bytes = written_frames * (uint32_t)block_align;
  FILE *stream = fopen(path, "wb");
  uint32_t frame;

  if (stream == NULL) {
    return 0;
  }
  if (!write_bytes(stream, "RIFF", 4u) ||
      !write_u32le(stream, 36u + data_bytes) ||
      !write_bytes(stream, "WAVE", 4u) ||
      !write_bytes(stream, "fmt ", 4u) || !write_u32le(stream, 16u) ||
      !write_u16le(stream, encoding) || !write_u16le(stream, channels) ||
      !write_u32le(stream, sample_rate) || !write_u32le(stream, byte_rate) ||
      !write_u16le(stream, block_align) ||
      !write_u16le(stream, bits_per_sample) ||
      !write_bytes(stream, "data", 4u) || !write_u32le(stream, data_bytes)) {
    fclose(stream);
    return 0;
  }

  if (encoding == 1u || (encoding == 3u && bits_per_sample == 32u)) {
    for (frame = 0; frame < written_frames; frame++) {
      uint16_t channel;
      for (channel = 0; channel < channels; channel++) {
        const int16_t sample = fixture_sample(frame, channel);
        int ok = 0;
        if (encoding == 1u && bits_per_sample == 16u) {
          ok = write_u16le(stream, (uint16_t)sample);
        } else if (encoding == 1u && bits_per_sample == 24u) {
          const uint32_t value = (uint32_t)((int32_t)sample * 256);
          const unsigned char bytes[3] = {
              (unsigned char)(value & 0xffu),
              (unsigned char)((value >> 8u) & 0xffu),
              (unsigned char)((value >> 16u) & 0xffu)};
          ok = write_bytes(stream, bytes, sizeof(bytes));
        } else if (encoding == 1u && bits_per_sample == 32u) {
          const uint32_t value = (uint32_t)((int64_t)sample * 65536);
          ok = write_u32le(stream, value);
        } else {
          ok = write_u32le(stream, fixture_float_bits(sample));
        }
        if (!ok) {
          fclose(stream);
          return 0;
        }
      }
    }
  } else {
    uint32_t index;
    for (index = 0; index < written_bytes; index++) {
      if (fputc(0, stream) == EOF) {
        fclose(stream);
        return 0;
      }
    }
  }

  return fclose(stream) == 0;
}

static int write_extensible_pcm16(const char *path,
                                  uint16_t extension_size,
                                  uint32_t frames) {
  static const unsigned char pcm_guid[16] = {
      0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u,
      0x80u, 0x00u, 0x00u, 0xaau, 0x00u, 0x38u, 0x9bu, 0x71u};
  const uint16_t channels = 1u;
  const uint32_t sample_rate = 8000u;
  const uint16_t block_align = 2u;
  const uint32_t data_bytes = frames * (uint32_t)block_align;
  FILE *stream = fopen(path, "wb");
  uint32_t frame;

  if (stream == NULL) {
    return 0;
  }
  if (!write_bytes(stream, "RIFF", 4u) ||
      !write_u32le(stream, 60u + data_bytes) ||
      !write_bytes(stream, "WAVE", 4u) ||
      !write_bytes(stream, "fmt ", 4u) || !write_u32le(stream, 40u) ||
      !write_u16le(stream, 0xfffeu) || !write_u16le(stream, channels) ||
      !write_u32le(stream, sample_rate) ||
      !write_u32le(stream, sample_rate * (uint32_t)block_align) ||
      !write_u16le(stream, block_align) || !write_u16le(stream, 16u) ||
      !write_u16le(stream, extension_size) || !write_u16le(stream, 16u) ||
      !write_u32le(stream, 0u) ||
      !write_bytes(stream, pcm_guid, sizeof(pcm_guid)) ||
      !write_bytes(stream, "data", 4u) || !write_u32le(stream, data_bytes)) {
    fclose(stream);
    return 0;
  }
  for (frame = 0u; frame < frames; frame++) {
    if (!write_u16le(stream, (uint16_t)fixture_sample(frame, 0u))) {
      fclose(stream);
      return 0;
    }
  }
  return fclose(stream) == 0;
}

static int replace_u32le(const char *path, long offset, uint32_t value) {
  FILE *stream = fopen(path, "r+b");
  int ok;
  if (stream == NULL || fseek(stream, offset, SEEK_SET) != 0) {
    if (stream != NULL) {
      fclose(stream);
    }
    return 0;
  }
  ok = write_u32le(stream, value);
  if (fclose(stream) != 0) {
    ok = 0;
  }
  return ok;
}

#if !defined(_WIN32)
static int write_sparse_large_riff(const char *path) {
  const off_t format_offset = (off_t)UINT64_C(0x80000014);
  FILE *stream = fopen(path, "wb");
  int ok = 1;

  if (stream == NULL) {
    return 0;
  }
  if (!write_bytes(stream, "RIFF", 4u) ||
      !write_u32le(stream, UINT32_C(0x8000002c)) ||
      !write_bytes(stream, "WAVE", 4u) ||
      !write_bytes(stream, "JUNK", 4u) ||
      !write_u32le(stream, UINT32_C(0x80000000)) ||
      fseeko(stream, format_offset, SEEK_SET) != 0 ||
      !write_bytes(stream, "fmt ", 4u) || !write_u32le(stream, 16u) ||
      !write_u16le(stream, 1u) || !write_u16le(stream, 1u) ||
      !write_u32le(stream, 8000u) || !write_u32le(stream, 16000u) ||
      !write_u16le(stream, 2u) || !write_u16le(stream, 16u) ||
      !write_bytes(stream, "data", 4u) || !write_u32le(stream, 0u)) {
    ok = 0;
  }
  if (fclose(stream) != 0) {
    ok = 0;
  }
  return ok;
}
#endif

static int write_corrupt_file(const char *path) {
  static const unsigned char contents[] = {
      'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v', 'e', '\n'};
  FILE *stream = fopen(path, "wb");
  int ok;
  if (stream == NULL) {
    return 0;
  }
  ok = write_bytes(stream, contents, sizeof(contents));
  if (fclose(stream) != 0) {
    ok = 0;
  }
  return ok;
}

#if !defined(_WIN32)
static int append_text(char *buffer,
                       size_t capacity,
                       size_t *length,
                       const char *text) {
  const size_t added = strlen(text);
  if (*length > capacity || added >= capacity - *length) {
    return 0;
  }
  memcpy(buffer + *length, text, added);
  *length += added;
  buffer[*length] = '\0';
  return 1;
}

static int append_shell_argument(char *buffer,
                                 size_t capacity,
                                 size_t *length,
                                 const char *argument) {
  const unsigned char *cursor = (const unsigned char *)argument;
#if defined(_WIN32)
  if (!append_text(buffer, capacity, length, "\"")) {
    return 0;
  }
  while (*cursor != '\0') {
    char character[2] = {(char)*cursor, '\0'};
    if (*cursor == '"') {
      if (!append_text(buffer, capacity, length, "\\\"")) {
        return 0;
      }
    } else if (!append_text(buffer, capacity, length, character)) {
      return 0;
    }
    cursor++;
  }
  return append_text(buffer, capacity, length, "\"");
#else
  if (!append_text(buffer, capacity, length, "'")) {
    return 0;
  }
  while (*cursor != '\0') {
    char character[2] = {(char)*cursor, '\0'};
    if (*cursor == '\'') {
      if (!append_text(buffer, capacity, length, "'\\''")) {
        return 0;
      }
    } else if (!append_text(buffer, capacity, length, character)) {
      return 0;
    }
    cursor++;
  }
  return append_text(buffer, capacity, length, "'");
#endif
}
#endif

static int run_analyzer_to(const char *stdout_path,
                           const char *stderr_path,
                           const char *const *arguments,
                           size_t argument_count) {
#if defined(_WIN32)
  return hwa_test_spawn_redirected(g_analyzer, arguments, argument_count,
                                   NULL, stdout_path, stderr_path);
#else
  char command[PATH_MAX * 4u];
  size_t length = 0u;
  size_t index;
  int status;

  command[0] = '\0';
  if (!append_shell_argument(command, sizeof(command), &length, g_analyzer)) {
    return -1;
  }
  for (index = 0; index < argument_count; index++) {
    if (!append_text(command, sizeof(command), &length, " ") ||
        !append_shell_argument(
            command, sizeof(command), &length, arguments[index])) {
      return -1;
    }
  }
  if (stdout_path != NULL &&
      (!append_text(command, sizeof(command), &length, " >") ||
      !append_shell_argument(
          command, sizeof(command), &length, stdout_path))) {
    return -1;
  }
  if (stderr_path != NULL &&
      (!append_text(command, sizeof(command), &length, " 2>") ||
      !append_shell_argument(
          command, sizeof(command), &length, stderr_path))) {
    return -1;
  }

  status = system(command);
  if (status == -1) {
    return -1;
  }
  if (!WIFEXITED(status)) {
    return 128;
  }
  return WEXITSTATUS(status);
#endif
}

static int run_analyzer(const test_workspace *workspace,
                        const char *const *arguments,
                        size_t argument_count) {
  return run_analyzer_to(workspace->stdout_path,
                         workspace->stderr_path,
                         arguments,
                         argument_count);
}

#if !defined(_WIN32)
static int run_analyzer_with_broken_stdout(const test_workspace *workspace,
                                           const char *const *arguments,
                                           size_t argument_count) {
  int output_pipe[2];
  pid_t child;
  pid_t waited;
  int child_status;

  if (argument_count > 6u || pipe(output_pipe) != 0) {
    return -1;
  }
  if (close(output_pipe[0]) != 0) {
    (void)close(output_pipe[1]);
    return -1;
  }
  child = fork();
  if (child < 0) {
    (void)close(output_pipe[1]);
    return -1;
  }
  if (child == 0) {
    char *child_arguments[8];
    int error_file;
    size_t index;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    (void)close(output_pipe[1]);
    error_file = open(workspace->stderr_path,
                      O_WRONLY | O_CREAT | O_TRUNC,
                      0600);
    if (error_file < 0 || dup2(error_file, STDERR_FILENO) < 0) {
      _exit(126);
    }
    (void)close(error_file);
    child_arguments[0] = (char *)g_analyzer;
    for (index = 0u; index < argument_count; index++) {
      child_arguments[index + 1u] = (char *)arguments[index];
    }
    child_arguments[argument_count + 1u] = NULL;
    (void)execv(g_analyzer, child_arguments);
    _exit(127);
  }
  (void)close(output_pipe[1]);
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) {
    return -1;
  }
  if (!WIFEXITED(child_status)) {
    return 128;
  }
  return WEXITSTATUS(child_status);
}
#endif

static char *read_whole_file(const char *path) {
  FILE *stream = fopen(path, "rb");
  long size;
  char *contents;

  if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
    if (stream != NULL) {
      fclose(stream);
    }
    return NULL;
  }
  size = ftell(stream);
  if (size < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
    fclose(stream);
    return NULL;
  }
  contents = (char *)malloc((size_t)size + 1u);
  if (contents == NULL) {
    fclose(stream);
    return NULL;
  }
  if (fread(contents, 1, (size_t)size, stream) != (size_t)size) {
    free(contents);
    fclose(stream);
    return NULL;
  }
  contents[size] = '\0';
  fclose(stream);
  return contents;
}

static int has_bad_number(const char *text) {
  const char *cursor = text;
  while (*cursor != '\0') {
    const int at_word_start = cursor == text ||
                              !isalpha((unsigned char)cursor[-1]);
    if (at_word_start && cursor[1] != '\0' && cursor[2] != '\0' &&
        (cursor[0] == 'n' || cursor[0] == 'N') &&
        (cursor[1] == 'a' || cursor[1] == 'A') &&
        (cursor[2] == 'n' || cursor[2] == 'N') &&
        !isalpha((unsigned char)cursor[3])) {
      return 1;
    }
    if (at_word_start && cursor[1] != '\0' && cursor[2] != '\0' &&
        (cursor[0] == 'i' || cursor[0] == 'I') &&
        (cursor[1] == 'n' || cursor[1] == 'N') &&
        (cursor[2] == 'f' || cursor[2] == 'F') &&
        !isalpha((unsigned char)cursor[3])) {
      return 1;
    }
    cursor++;
  }
  return 0;
}

#if !defined(_WIN32)
static size_t utf8_sequence_size(const unsigned char *text) {
  const unsigned char first = text[0];
  const unsigned char second = text[1];

  if (first < 0x80u) {
    return 1u;
  }
  if (second == 0u) {
    return 0u;
  }
  if (first >= 0xc2u && first <= 0xdfu) {
    return second >= 0x80u && second <= 0xbfu ? 2u : 0u;
  }
  if (text[2] == 0u) {
    return 0u;
  }
  if (first == 0xe0u) {
    return second >= 0xa0u && second <= 0xbfu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu
               ? 3u
               : 0u;
  }
  if (first >= 0xe1u && first <= 0xecu) {
    return second >= 0x80u && second <= 0xbfu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu
               ? 3u
               : 0u;
  }
  if (first == 0xedu) {
    return second >= 0x80u && second <= 0x9fu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu
               ? 3u
               : 0u;
  }
  if (first >= 0xeeu && first <= 0xefu) {
    return second >= 0x80u && second <= 0xbfu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu
               ? 3u
               : 0u;
  }
  if (text[3] == 0u) {
    return 0u;
  }
  if (first == 0xf0u) {
    return second >= 0x90u && second <= 0xbfu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu && text[3] >= 0x80u && text[3] <= 0xbfu
               ? 4u
               : 0u;
  }
  if (first >= 0xf1u && first <= 0xf3u) {
    return second >= 0x80u && second <= 0xbfu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu && text[3] >= 0x80u && text[3] <= 0xbfu
               ? 4u
               : 0u;
  }
  if (first == 0xf4u) {
    return second >= 0x80u && second <= 0x8fu && text[2] >= 0x80u &&
                   text[2] <= 0xbfu && text[3] >= 0x80u && text[3] <= 0xbfu
               ? 4u
               : 0u;
  }
  return 0u;
}

static int text_is_valid_utf8(const char *text) {
  const unsigned char *cursor = (const unsigned char *)text;
  while (*cursor != 0u) {
    const size_t sequence_size = utf8_sequence_size(cursor);
    if (sequence_size == 0u) {
      return 0;
    }
    cursor += sequence_size;
  }
  return 1;
}

static char *hex_bytes(const char *text) {
  static const char digits[] = "0123456789abcdef";
  const unsigned char *bytes = (const unsigned char *)text;
  const size_t length = strlen(text);
  char *hex;
  size_t index;

  if (length > (SIZE_MAX - 1u) / 2u) {
    return NULL;
  }
  hex = (char *)malloc(length * 2u + 1u);
  if (hex == NULL) {
    return NULL;
  }
  for (index = 0u; index < length; index++) {
    hex[index * 2u] = digits[bytes[index] >> 4u];
    hex[index * 2u + 1u] = digits[bytes[index] & 0x0fu];
  }
  hex[length * 2u] = '\0';
  return hex;
}
#endif

static const char *json_value(const char *json, const char *key) {
  char token[128];
  const char *cursor;
  const int count = snprintf(token, sizeof(token), "\"%s\"", key);
  if (count < 0 || (size_t)count >= sizeof(token)) {
    return NULL;
  }
  cursor = strstr(json, token);
  if (cursor == NULL) {
    return NULL;
  }
  cursor += strlen(token);
  while (isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if (*cursor++ != ':') {
    return NULL;
  }
  while (isspace((unsigned char)*cursor)) {
    cursor++;
  }
  return cursor;
}

static int json_string_is(const char *json,
                          const char *key,
                          const char *expected) {
  const char *value = json_value(json, key);
  const size_t length = strlen(expected);
  return value != NULL && *value == '"' &&
         strncmp(value + 1, expected, length) == 0 &&
         value[length + 1u] == '"';
}

static int json_number(const char *json, const char *key, double *result) {
  const char *value = json_value(json, key);
  char *end;
  if (value == NULL) {
    return 0;
  }
  errno = 0;
  *result = strtod(value, &end);
  return end != value && errno != ERANGE && isfinite(*result);
}

static int near_value(double actual, double expected, double tolerance) {
  return fabs(actual - expected) <= tolerance;
}

static uint64_t hash_file(const char *path, int *ok) {
  FILE *stream = fopen(path, "rb");
  uint64_t hash = UINT64_C(1469598103934665603);
  unsigned char buffer[4096];
  size_t count;
  if (stream == NULL) {
    *ok = 0;
    return 0u;
  }
  while ((count = fread(buffer, 1, sizeof(buffer), stream)) != 0u) {
    size_t index;
    for (index = 0; index < count; index++) {
      hash ^= (uint64_t)buffer[index];
      hash *= UINT64_C(1099511628211);
    }
  }
  *ok = ferror(stream) == 0 && fclose(stream) == 0;
  return hash;
}

static int snapshot_file(const char *path, file_snapshot *snapshot) {
#if defined(_WIN32)
  struct _stat64 status;
#else
  struct stat status;
#endif
  int hash_ok;
#if defined(_WIN32)
  if (_stat64(path, &status) != 0) {
#else
  if (stat(path, &status) != 0) {
#endif
    return 0;
  }
  snapshot->size = (int64_t)status.st_size;
  snapshot->seconds = status.st_mtime;
#if defined(_WIN32)
  snapshot->nanoseconds = 0L;
#elif defined(__APPLE__)
  snapshot->nanoseconds = status.st_mtimensec;
#else
  snapshot->nanoseconds = status.st_mtim.tv_nsec;
#endif
  snapshot->hash = hash_file(path, &hash_ok);
  return hash_ok;
}

static int snapshots_equal(const file_snapshot *before,
                           const file_snapshot *after) {
  return before->size == after->size && before->seconds == after->seconds &&
         before->nanoseconds == after->nanoseconds && before->hash == after->hash;
}

static void expect_success(int status, const test_workspace *workspace) {
  char *errors = read_whole_file(workspace->stderr_path);
  CHECK(status == 0, "expected exit 0, got %d; stderr: %s", status,
        errors != NULL ? errors : "<unreadable>");
  free(errors);
}

static void expect_data_failure(int status, const test_workspace *workspace) {
  char *errors = read_whole_file(workspace->stderr_path);
  CHECK(status == 1, "expected data-error exit 1, got %d", status);
  CHECK(errors != NULL && errors[0] != '\0',
        "data failure should explain the error on stderr");
  free(errors);
}

static int case_mono_text(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  char *output;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "mono.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 1u, 1u, 8000u, 16u, 800u, 800u),
        "could not write mono fixture");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  CHECK(output != NULL && output[0] != '\0', "text inspect output is empty");
  CHECK(output != NULL && strstr(output, "mono.wav") != NULL,
        "text inspect output should name the input");
  CHECK(output != NULL && strstr(output, "File:") != NULL &&
            strstr(output, "Encoding:") != NULL &&
            strstr(output, "Channels:") != NULL &&
            strstr(output, "Sample rate:") != NULL &&
            strstr(output, "Bits per sample:") != NULL &&
            strstr(output, "Frames:") != NULL &&
            strstr(output, "Duration:") != NULL &&
            strstr(output, "Channel 1:") != NULL,
        "text inspect output is missing stable field labels");
  CHECK(output != NULL && strstr(output, "8000") != NULL,
        "text inspect output should include the sample rate");
  CHECK(output != NULL && strstr(output, "800") != NULL,
        "text inspect output should include the frame count");
  CHECK(output != NULL && !has_bad_number(output),
        "text inspect output contains NaN or infinity");
  free(output);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_mono_json(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[3];
  char *output;
  double value = 0.0;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "mono.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 1u, 1u, 8000u, 16u, 800u, 800u),
        "could not write mono fixture");
  arguments[0] = "--json";
  arguments[1] = "inspect";
  arguments[2] = input;
  status = run_analyzer(&workspace, arguments, 3u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  CHECK(output != NULL && json_string_is(output, "command", "inspect"),
        "JSON command should be inspect");
  CHECK(output != NULL && strstr(output, "\"schema_version\"") != NULL,
        "JSON should have a schema version");
  CHECK(output != NULL && json_number(output, "channels", &value) &&
            near_value(value, 1.0, 0.0),
        "JSON channels should be 1");
  CHECK(output != NULL && json_number(output, "sample_rate_hz", &value) &&
            near_value(value, 8000.0, 0.0),
        "JSON sample rate should be 8000 Hz");
  CHECK(output != NULL && json_number(output, "bits_per_sample", &value) &&
            near_value(value, 16.0, 0.0),
        "JSON bit depth should be 16");
  CHECK(output != NULL && json_number(output, "frames", &value) &&
            near_value(value, 800.0, 0.0),
        "JSON frame count should be 800");
  CHECK(output != NULL && json_number(output, "duration_seconds", &value) &&
            near_value(value, 0.1, 1e-12),
        "JSON duration should be 0.1 seconds");
  CHECK(output != NULL && json_number(output, "peak", &value) &&
            near_value(value, 0.5, 1e-6),
        "JSON peak should be 0.5, got %.12g", value);
  CHECK(output != NULL && json_number(output, "rms", &value) &&
            near_value(value, sqrt(0.09375), 1e-6),
        "JSON RMS is outside tolerance, got %.12g", value);
  CHECK(output != NULL && !has_bad_number(output),
        "JSON contains NaN or infinity");
  free(output);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

#if !defined(_WIN32)
static int case_invalid_utf8_path_json(void) {
  static const unsigned char suffix[] = {
      'p', 'a', 't', 'h', '-', '\'', '"', '\\', '\n', '-',
      0xc3u, 0xa9u, '-', 0xffu, '.', 'w', 'a', 'v', 0u};
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[3];
  char *expected_hex;
  char *output;
  size_t prefix_size;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  prefix_size = strlen(workspace.directory);
  CHECK(prefix_size + 1u + sizeof(suffix) <= sizeof(input),
        "invalid UTF-8 fixture path is too long");
  if (g_failures != 0) {
    workspace_close(&workspace);
    return 0;
  }
  memcpy(input, workspace.directory, prefix_size);
  input[prefix_size] = '/';
  memcpy(input + prefix_size + 1u, suffix, sizeof(suffix));
  if (!write_wave(input, 1u, 1u, 8000u, 16u, 80u, 80u)) {
    g_skipped = 1;
    (void)remove_file(input);
    workspace_close(&workspace);
    return 0;
  }
  arguments[0] = "--json";
  arguments[1] = "inspect";
  arguments[2] = input;
  status = run_analyzer(&workspace, arguments, 3u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  expected_hex = hex_bytes(input);
  CHECK(output != NULL && text_is_valid_utf8(output),
        "JSON output is not valid UTF-8");
  CHECK(output != NULL && strstr(output, "\xc3\xa9") != NULL,
        "valid UTF-8 path bytes should remain UTF-8");
  CHECK(output != NULL && strstr(output, "\\u00ff") != NULL,
        "invalid path byte should use a lowercase \\u00xx escape");
  CHECK(output != NULL && memchr(output, 0xff, strlen(output)) == NULL,
        "invalid path byte leaked into JSON output");
  CHECK(output != NULL && strstr(output, "\\\"") != NULL &&
            strstr(output, "\\\\") != NULL &&
            strstr(output, "\\n") != NULL,
        "JSON path did not escape quote, backslash, or newline");
  CHECK(output != NULL &&
            json_string_is(output,
                           "path_encoding",
                           "utf8_with_invalid_bytes_as_u00xx"),
        "JSON should state its path encoding rule");
  CHECK(output != NULL && expected_hex != NULL &&
            json_string_is(output, "path_bytes_hex", expected_hex),
        "path_bytes_hex should preserve the exact POSIX filename bytes");
  CHECK(output != NULL && output[0] == '{' &&
            strlen(output) >= 2u && output[strlen(output) - 2u] == '}',
        "JSON output should be one complete object");
  free(expected_hex);
  free(output);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}
#endif

static int case_stereo_json(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[3];
  char *output;
  double value = 0.0;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "stereo.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 1u, 2u, 16000u, 16u, 1600u, 1600u),
        "could not write stereo fixture");
  arguments[0] = "inspect";
  arguments[1] = "--json";
  arguments[2] = input;
  status = run_analyzer(&workspace, arguments, 3u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  CHECK(output != NULL && json_string_is(output, "command", "inspect"),
        "JSON command should be inspect");
  CHECK(output != NULL && json_number(output, "channels", &value) &&
            near_value(value, 2.0, 0.0),
        "JSON channels should be 2");
  CHECK(output != NULL && strstr(output, "\"stereo\"") != NULL &&
            strstr(output, "\"correlation\"") != NULL,
        "stereo JSON should include correlation data");
  CHECK(output != NULL && json_number(output, "correlation", &value) &&
            value >= -1.000001 && value <= 1.000001 &&
            near_value(value, -1.0, 1e-6),
        "stereo correlation should be finite and near -1, got %.12g", value);
  CHECK(output != NULL && json_number(output, "mid_rms", &value) && value > 0.0,
        "mid RMS should be positive and finite");
  CHECK(output != NULL && json_number(output, "side_rms", &value) && value > 0.0,
        "side RMS should be positive and finite");
  CHECK(output != NULL && !has_bad_number(output),
        "stereo JSON contains NaN or infinity");
  free(output);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_supported_formats(void) {
  typedef struct {
    const char *name;
    uint16_t encoding;
    uint16_t bits;
    int extensible;
  } format_case;
  static const format_case formats[] = {
      {"pcm24.wav", 1u, 24u, 0},
      {"pcm32.wav", 1u, 32u, 0},
      {"float32.wav", 3u, 32u, 0},
      {"extensible-pcm16.wav", 1u, 16u, 1},
  };
  test_workspace workspace;
  size_t index;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  for (index = 0u; index < sizeof(formats) / sizeof(formats[0]); index++) {
    char input[PATH_MAX];
    const char *arguments[3];
    char *output;
    double value = 0.0;
    int status;

    CHECK(join_path(input,
                    sizeof(input),
                    workspace.directory,
                    formats[index].name),
          "fixture path is too long");
    if (formats[index].extensible) {
      CHECK(write_extensible_pcm16(input, 22u, 800u),
            "could not write extensible fixture");
    } else {
      CHECK(write_wave(input,
                       formats[index].encoding,
                       1u,
                       8000u,
                       formats[index].bits,
                       800u,
                       800u),
            "could not write %s fixture", formats[index].name);
    }
    arguments[0] = "--json";
    arguments[1] = "inspect";
    arguments[2] = input;
    status = run_analyzer(&workspace, arguments, 3u);
    expect_success(status, &workspace);
    output = read_whole_file(workspace.stdout_path);
    CHECK(output != NULL && json_number(output, "bits_per_sample", &value) &&
              near_value(value, (double)formats[index].bits, 0.0),
          "%s JSON has the wrong bit depth", formats[index].name);
    CHECK(output != NULL &&
              json_string_is(output,
                             "encoding",
                             formats[index].encoding == 1u ? "pcm"
                                                           : "ieee_float"),
          "%s JSON has the wrong encoding", formats[index].name);
    CHECK(output != NULL && json_number(output, "peak", &value) &&
              near_value(value, 0.5, 1e-6),
          "%s peak is outside tolerance", formats[index].name);
    CHECK(output != NULL && !has_bad_number(output),
          "%s JSON contains NaN or infinity", formats[index].name);
    free(output);
    (void)remove_file(input);
  }
  workspace_close(&workspace);
  return g_failures == 0;
}

#if !defined(_WIN32)
static int case_sparse_large_riff(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[3];
  char *output;
  double value = -1.0;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "sparse-large.wav"),
        "sparse fixture path is too long");
  if (!write_sparse_large_riff(input)) {
    g_skipped = 1;
    (void)remove_file(input);
    workspace_close(&workspace);
    return 0;
  }
  arguments[0] = "--json";
  arguments[1] = "inspect";
  arguments[2] = input;
  status = run_analyzer(&workspace, arguments, 3u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  CHECK(output != NULL && json_number(output, "frames", &value) &&
            near_value(value, 0.0, 0.0),
        "sparse RIFF should report zero sample frames");
  CHECK(output != NULL && json_number(output, "data_bytes", &value) &&
            near_value(value, 0.0, 0.0),
        "sparse RIFF should report a zero-byte data chunk");
  CHECK(output != NULL && json_number(output, "duration_seconds", &value) &&
            near_value(value, 0.0, 0.0),
        "sparse RIFF should report zero duration");
  free(output);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}
#endif

static int case_compare_unequal_json(void) {
  test_workspace workspace;
  char reference[PATH_MAX];
  char model[PATH_MAX];
  const char *arguments[4];
  const char *delta;
  char *output;
  double value = 0.0;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(reference,
                  sizeof(reference),
                  workspace.directory,
                  "reference.wav"),
        "reference path is too long");
  CHECK(join_path(model, sizeof(model), workspace.directory, "model.wav"),
        "model path is too long");
  CHECK(write_wave(reference, 1u, 1u, 8000u, 16u, 8000u, 8000u),
        "could not write reference fixture");
  CHECK(write_wave(model, 1u, 1u, 8000u, 16u, 4000u, 4000u),
        "could not write model fixture");
  arguments[0] = "compare";
  arguments[1] = "--json";
  arguments[2] = reference;
  arguments[3] = model;
  status = run_analyzer(&workspace, arguments, 4u);
  expect_success(status, &workspace);
  output = read_whole_file(workspace.stdout_path);
  CHECK(output != NULL && json_string_is(output, "command", "compare"),
        "JSON command should be compare");
  CHECK(output != NULL && strstr(output, "\"reference\"") != NULL &&
            strstr(output, "\"model\"") != NULL,
        "compare JSON should describe both inputs");
  delta = output != NULL ? json_value(output, "delta") : NULL;
  CHECK(delta != NULL && *delta == '{', "compare JSON should have a delta object");
  CHECK(delta != NULL && json_number(delta, "duration_seconds", &value) &&
            near_value(value, -0.5, 1e-12),
        "duration delta should be model-reference (-0.5), got %.12g", value);
  CHECK(delta != NULL && json_number(delta, "frames", &value) &&
            near_value(value, -4000.0, 0.0),
        "frame delta should be model-reference (-4000), got %.12g", value);
  CHECK(output != NULL && !has_bad_number(output),
        "compare JSON contains NaN or infinity");
  free(output);
  (void)remove_file(reference);
  (void)remove_file(model);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_reject_corrupt(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "corrupt.wav"),
        "fixture path is too long");
  CHECK(write_corrupt_file(input), "could not write corrupt fixture");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_reject_truncated(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "truncated.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 1u, 1u, 8000u, 16u, 800u, 4u),
        "could not write truncated fixture");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_reject_unsupported(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "alaw.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 6u, 1u, 8000u, 8u, 80u, 80u),
        "could not write unsupported fixture");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_reject_nonfinite_float(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "nan.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 3u, 1u, 8000u, 32u, 80u, 80u),
        "could not write float fixture");
  CHECK(replace_u32le(input, 44L + 12L, UINT32_C(0x7fc00000)),
        "could not insert non-finite float sample");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_reject_bad_extensible_size(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input,
                  sizeof(input),
                  workspace.directory,
                  "bad-extensible-size.wav"),
        "fixture path is too long");
  CHECK(write_extensible_pcm16(input, 23u, 80u),
        "could not write malformed extensible fixture");
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

#if !defined(_WIN32)
static int case_reject_fifo(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[2];
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "input.fifo"),
        "FIFO path is too long");
  CHECK(mkfifo(input, 0600) == 0, "could not create FIFO fixture: %s",
        strerror(errno));
  arguments[0] = "inspect";
  arguments[1] = input;
  status = run_analyzer(&workspace, arguments, 2u);
  expect_data_failure(status, &workspace);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}

static int case_output_failure(void) {
  test_workspace workspace;
  char input[PATH_MAX];
  const char *arguments[3];
  char *errors;
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(input, sizeof(input), workspace.directory, "mono.wav"),
        "fixture path is too long");
  CHECK(write_wave(input, 1u, 1u, 8000u, 16u, 80u, 80u),
        "could not write output-failure fixture");
  arguments[0] = "--json";
  arguments[1] = "inspect";
  arguments[2] = input;
  status = run_analyzer_with_broken_stdout(&workspace, arguments, 3u);
  errors = read_whole_file(workspace.stderr_path);
  CHECK(status == 1, "stdout write failure should return 1, got %d", status);
  CHECK(errors != NULL &&
            strstr(errors, "cannot write standard output") != NULL,
        "stdout write failure should have a clear diagnostic");
  free(errors);
  (void)remove_file(input);
  workspace_close(&workspace);
  return g_failures == 0;
}
#endif

static int case_read_only(void) {
  test_workspace workspace;
  char reference[PATH_MAX];
  char model[PATH_MAX];
  const char *inspect_arguments[3];
  const char *compare_arguments[4];
  file_snapshot reference_before = {0};
  file_snapshot reference_after = {0};
  file_snapshot model_before = {0};
  file_snapshot model_after = {0};
  int status;

  CHECK(workspace_open(&workspace), "could not create workspace");
  if (g_failures != 0) {
    return 0;
  }
  CHECK(join_path(reference,
                  sizeof(reference),
                  workspace.directory,
                  "reference.wav"),
        "reference path is too long");
  CHECK(join_path(model, sizeof(model), workspace.directory, "model.wav"),
        "model path is too long");
  CHECK(write_wave(reference, 1u, 2u, 44100u, 16u, 4410u, 4410u),
        "could not write reference fixture");
  CHECK(write_wave(model, 1u, 1u, 44100u, 16u, 2205u, 2205u),
        "could not write model fixture");
  CHECK(make_file_read_only(reference) == 0,
        "could not make reference read-only");
  CHECK(make_file_read_only(model) == 0, "could not make model read-only");
  CHECK(snapshot_file(reference, &reference_before),
        "could not snapshot reference input");
  CHECK(snapshot_file(model, &model_before), "could not snapshot model input");

  inspect_arguments[0] = "--json";
  inspect_arguments[1] = "inspect";
  inspect_arguments[2] = reference;
  status = run_analyzer(&workspace, inspect_arguments, 3u);
  expect_success(status, &workspace);
  compare_arguments[0] = "--json";
  compare_arguments[1] = "compare";
  compare_arguments[2] = reference;
  compare_arguments[3] = model;
  status = run_analyzer(&workspace, compare_arguments, 4u);
  expect_success(status, &workspace);

  CHECK(snapshot_file(reference, &reference_after),
        "could not snapshot reference after analysis");
  CHECK(snapshot_file(model, &model_after),
        "could not snapshot model after analysis");
  CHECK(snapshots_equal(&reference_before, &reference_after),
        "inspect or compare changed reference bytes, size, or mtime");
  CHECK(snapshots_equal(&model_before, &model_after),
        "compare changed model bytes, size, or mtime");

  (void)make_file_writable(reference);
  (void)make_file_writable(model);
  (void)remove_file(reference);
  (void)remove_file(model);
  workspace_close(&workspace);
  return g_failures == 0;
}

typedef int (*test_function)(void);

typedef struct {
  const char *name;
  test_function function;
} test_case;

static const test_case g_cases[] = {
    {"mono-text", case_mono_text},
    {"mono-json", case_mono_json},
#if !defined(_WIN32)
    {"invalid-utf8-path-json", case_invalid_utf8_path_json},
#endif
    {"stereo-json", case_stereo_json},
    {"supported-formats", case_supported_formats},
#if !defined(_WIN32)
    {"sparse-large-riff", case_sparse_large_riff},
#endif
    {"compare-unequal-json", case_compare_unequal_json},
    {"reject-corrupt", case_reject_corrupt},
    {"reject-truncated", case_reject_truncated},
    {"reject-unsupported", case_reject_unsupported},
    {"reject-nonfinite-float", case_reject_nonfinite_float},
    {"reject-bad-extensible-size", case_reject_bad_extensible_size},
#if !defined(_WIN32)
    {"reject-fifo", case_reject_fifo},
    {"output-failure", case_output_failure},
#endif
    {"read-only", case_read_only},
};

static void print_usage(const char *program) {
  size_t index;
  fprintf(stderr, "usage: %s ANALYZER CASE\n", program);
  fputs("cases:", stderr);
  for (index = 0; index < sizeof(g_cases) / sizeof(g_cases[0]); index++) {
    fprintf(stderr, " %s", g_cases[index].name);
  }
  fputs(" all\n", stderr);
}

int main(int argc, char **argv) {
  size_t index;
  int matched = 0;

  if (argc != 3) {
    print_usage(argv[0]);
    return 2;
  }
  g_analyzer = argv[1];
  for (index = 0; index < sizeof(g_cases) / sizeof(g_cases[0]); index++) {
    if (strcmp(argv[2], "all") == 0 || strcmp(argv[2], g_cases[index].name) == 0) {
      matched = 1;
      g_failures = 0;
      g_skipped = 0;
      if (!g_cases[index].function() || g_failures != 0) {
        if (g_skipped) {
          fprintf(stderr, "SKIP %s: host cannot create required fixture\n",
                  g_cases[index].name);
          return 77;
        }
        fprintf(stderr, "case %s failed with %d assertion(s)\n",
                g_cases[index].name,
                g_failures);
        return 1;
      }
      printf("PASS %s\n", g_cases[index].name);
    }
  }
  if (!matched) {
    print_usage(argv[0]);
    return 2;
  }
  return 0;
}
