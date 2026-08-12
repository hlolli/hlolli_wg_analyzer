#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
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
#include <windows.h>
#include "windows_test_process.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_RATE 8000U
#define TEST_FRAMES 16000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestFiles {
    char directory[PATH_MAX];
    char audio[PATH_MAX];
    char audio_copy[PATH_MAX];
    char changed_audio[PATH_MAX];
    char score[PATH_MAX];
    char labels[PATH_MAX];
    char bad_labels[PATH_MAX];
    char changed_labels[PATH_MAX];
    char alignment[PATH_MAX];
    char second_alignment[PATH_MAX];
    char audio_alignment[PATH_MAX];
    char items[PATH_MAX];
    char second_items[PATH_MAX];
    char output[PATH_MAX];
    char error[PATH_MAX];
} TestFiles;

typedef struct JsonCursor {
    const unsigned char *current;
    unsigned depth;
} JsonCursor;

static const char *analyzer_path;
static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static int test_make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int test_remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int test_remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int test_make_hard_link(const char *source, const char *alias)
{
#if defined(_WIN32)
    return CreateHardLinkA(alias, source, NULL) != 0;
#else
    return link(source, alias) == 0;
#endif
}

static int test_files_open(TestFiles *files)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif

    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(files->directory, sizeof(files->directory),
                              "%s/hwa-stage3-cli-%ld-%u",
                              root, test_process_id(), attempt);
        if (length < 0 || (size_t)length >= sizeof(files->directory)) return 0;
        if (test_make_directory(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt != 100U &&
           test_join(files->audio, files->directory, "audio.wav") &&
           test_join(files->audio_copy, files->directory, "audio-copy.wav") &&
           test_join(files->changed_audio, files->directory, "changed.wav") &&
           test_join(files->score, files->directory, "score.csv") &&
           test_join(files->labels, files->directory, "labels.csv") &&
           test_join(files->bad_labels, files->directory, "bad-labels.csv") &&
           test_join(files->changed_labels, files->directory,
                     "changed-labels.csv") &&
           test_join(files->alignment, files->directory, "score.hwa-align") &&
           test_join(files->second_alignment, files->directory,
                     "second.hwa-align") &&
           test_join(files->audio_alignment, files->directory,
                     "audio.hwa-align") &&
           test_join(files->items, files->directory, "items.hwa-items") &&
           test_join(files->second_items, files->directory,
                     "second.hwa-items") &&
           test_join(files->output, files->directory, "stdout.txt") &&
           test_join(files->error, files->directory, "stderr.txt");
}

static void test_files_close(TestFiles *files)
{
    static const char *const names[] = {
        "audio.wav", "audio-copy.wav", "changed.wav", "score.csv",
        "labels.csv", "bad-labels.csv", "changed-labels.csv",
        "score.hwa-align", "second.hwa-align", "audio.hwa-align",
        "items.hwa-items", "second.hwa-items", "stdout.txt", "stderr.txt",
        "other.hwa-items"
    };
    char path[PATH_MAX];
    size_t index;

    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (test_join(path, files->directory, names[index])) {
            (void)test_remove_file(path);
        }
    }
    (void)test_remove_directory(files->directory);
}

static int test_write_bytes(FILE *stream, const void *data, size_t size)
{
    return fwrite(data, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int16_t test_sample(uint32_t frame, int changed)
{
    static const double frequencies[4] = {
        261.6255653005986, 329.6275569128699,
        391.9954359817493, 293.6647679174076
    };
    uint32_t within = frame % 4000U;
    size_t note = (size_t)(frame / 4000U);
    double edge = (double)(within < 96U ? within :
                           (within > 3904U ? 4000U - within : 96U)) / 96.0;
    double frequency = frequencies[note];
    double value;

    if (changed != 0 && note == 1U) frequency *= 1.04;
    value = 0.50 * edge * sin(2.0 * TEST_PI * frequency *
                              (double)frame / (double)TEST_RATE);
    return (int16_t)lrint(value * 32767.0);
}

static int test_write_wav(const char *path, int changed)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) return 0;
    if (!test_write_bytes(stream, "RIFF", 4U) ||
        !test_write_u32(stream, 36U + TEST_FRAMES * 2U) ||
        !test_write_bytes(stream, "WAVE", 4U) ||
        !test_write_bytes(stream, "fmt ", 4U) ||
        !test_write_u32(stream, 16U) || !test_write_u16(stream, 1U) ||
        !test_write_u16(stream, 1U) || !test_write_u32(stream, TEST_RATE) ||
        !test_write_u32(stream, TEST_RATE * 2U) ||
        !test_write_u16(stream, 2U) || !test_write_u16(stream, 16U) ||
        !test_write_bytes(stream, "data", 4U) ||
        !test_write_u32(stream, TEST_FRAMES * 2U)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < TEST_FRAMES; ++frame) {
        if (!test_write_u16(stream, (uint16_t)test_sample(frame, changed))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int test_write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    int okay = stream != NULL && fwrite(text, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_copy_file(const char *source, const char *target)
{
    FILE *input = fopen(source, "rb");
    FILE *output = NULL;
    unsigned char buffer[4096];
    int okay = input != NULL;

    if (okay) {
        output = fopen(target, "wb");
        okay = output != NULL;
    }
    while (okay) {
        size_t count = fread(buffer, 1U, sizeof(buffer), input);
        if (count != 0U && fwrite(buffer, 1U, count, output) != count) {
            okay = 0;
        }
        if (count != sizeof(buffer)) {
            if (ferror(input)) okay = 0;
            break;
        }
    }
    if (output != NULL && fclose(output) != 0) okay = 0;
    if (input != NULL && fclose(input) != 0) okay = 0;
    return okay;
}

static int test_setup(TestFiles *files)
{
    static const char score[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n"
        "tempo,tempo,0,0,,,,,,,theme,120\n"
        "n1,note,0,1,60,88,solo,none,mf,,theme,\n"
        "n2,note,1,1,64,84,solo,none,p,,theme,\n"
        "n3,note,2,1,67,96,solo,none,f,,theme,\n"
        "n4,note,3,1,62,86,solo,none,mf,,theme,";
    static const char labels[] =
        "event_id,pitch,register,dynamic,articulation,part,physical_element,controller,technique,score_section,transition,gesture\n"
        "n1,C4,octave-4,mf,accent,lead,resonator-a,exciter,,theme,,\n"
        "n2,E4,octave-4,p,legato,lead,resonator-a,exciter,sustain,theme,blend,\n"
        "n3,G4,octave-4,f,staccato,lead,resonator-b,exciter,,theme,switch,\n"
        "n4,D4,octave-4,mf,,lead,resonator-c,gate,impulse,theme,,pulse";
    static const char bad_labels[] =
        "event_id,pitch,register,dynamic,articulation,part,physical_element,controller,technique,score_section,transition,gesture\n"
        "unknown,C4,octave-4,mf,,lead,resonator-a,exciter,,theme,,";
    static const char changed_labels[] =
        "event_id,pitch,register,dynamic,articulation,part,physical_element,controller,technique,score_section,transition,gesture\n"
        "n1,C4,octave-4,ff,accent,lead,resonator-a,exciter,,theme,,\n"
        "n2,E4,octave-4,p,legato,lead,resonator-a,exciter,sustain,theme,blend,\n"
        "n3,G4,octave-4,f,staccato,lead,resonator-b,exciter,,theme,switch,\n"
        "n4,D4,octave-4,mf,,lead,resonator-c,gate,impulse,theme,,pulse";

    CHECK(test_files_open(files), "could not make Stage 3 workspace");
    if (failures != 0) return 0;
    CHECK(test_write_wav(files->audio, 0) &&
              test_copy_file(files->audio, files->audio_copy) &&
              test_write_wav(files->changed_audio, 1) &&
              test_write_text(files->score, score) &&
              test_write_text(files->labels, labels) &&
              test_write_text(files->bad_labels, bad_labels) &&
              test_write_text(files->changed_labels, changed_labels),
          "could not write Stage 3 fixtures");
    return failures == 0;
}

#if !defined(_WIN32)
static int test_append(char *command,
                       size_t capacity,
                       size_t *length,
                       const char *text)
{
    size_t added = strlen(text);
    if (*length >= capacity || added >= capacity - *length) return 0;
    memcpy(command + *length, text, added + 1U);
    *length += added;
    return 1;
}

static int test_append_argument(char *command,
                                size_t capacity,
                                size_t *length,
                                const char *argument)
{
    const unsigned char *cursor = (const unsigned char *)argument;
#if defined(_WIN32)
    if (!test_append(command, capacity, length, "\"")) return 0;
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};
        if (*cursor == '"') {
            if (!test_append(command, capacity, length, "\\\"")) return 0;
        } else if (!test_append(command, capacity, length, character)) {
            return 0;
        }
        cursor++;
    }
    return test_append(command, capacity, length, "\"");
#else
    if (!test_append(command, capacity, length, "'")) return 0;
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};
        if (*cursor == '\'') {
            if (!test_append(command, capacity, length, "'\\''")) return 0;
        } else if (!test_append(command, capacity, length, character)) {
            return 0;
        }
        cursor++;
    }
    return test_append(command, capacity, length, "'");
#endif
}
#endif

static int test_run(const TestFiles *files,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(
        analyzer_path, arguments, argument_count, NULL,
        files->output, files->error);
#else
    char command[PATH_MAX * 12U];
    size_t length = 0U;
    size_t index;
    int status;

    command[0] = '\0';
    if (!test_append_argument(command, sizeof(command), &length,
                              analyzer_path)) return -1;
    for (index = 0U; index < argument_count; ++index) {
        if (!test_append(command, sizeof(command), &length, " ") ||
            !test_append_argument(command, sizeof(command), &length,
                                  arguments[index])) return -1;
    }
    if (!test_append(command, sizeof(command), &length, " >") ||
        !test_append_argument(command, sizeof(command), &length, files->output) ||
        !test_append(command, sizeof(command), &length, " 2>") ||
        !test_append_argument(command, sizeof(command), &length, files->error)) {
        return -1;
    }
    status = system(command);
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

#if !defined(_WIN32)
static int test_run_with_broken_stdout(const TestFiles *files,
                                       const char *const *arguments,
                                       size_t argument_count)
{
    char *exec_arguments[34];
    int output_pipe[2];
    pid_t child;
    pid_t waited;
    size_t index;
    int status;

    if (argument_count > 32U || pipe(output_pipe) != 0) return -1;
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
        int error_file;

        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        (void)close(output_pipe[1]);
        error_file = open(files->error,
                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (error_file < 0 || dup2(error_file, STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(error_file);
        exec_arguments[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index) {
            exec_arguments[index + 1U] = (char *)arguments[index];
        }
        exec_arguments[argument_count + 1U] = NULL;
        (void)execv(analyzer_path, exec_arguments);
        _exit(127);
    }
    (void)close(output_pipe[1]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
#endif

static unsigned char *test_read_file(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long end;
    unsigned char *data;
    int okay;

    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)end + 1U);
    if (data == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    okay = ((size_t)end == 0U ||
            fread(data, 1U, (size_t)end, stream) == (size_t)end);
    if (fclose(stream) != 0) okay = 0;
    if (!okay) {
        free(data);
        return NULL;
    }
    data[(size_t)end] = 0U;
    *size = (size_t)end;
    return data;
}

static int test_path_exists(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) return 0;
    (void)fclose(stream);
    return 1;
}

static int test_file_matches(const char *path,
                             const unsigned char *expected,
                             size_t expected_size)
{
    size_t actual_size;
    unsigned char *actual = test_read_file(path, &actual_size);
    int matches = actual != NULL && expected != NULL &&
                  actual_size == expected_size &&
                  memcmp(actual, expected, expected_size) == 0;
    free(actual);
    return matches;
}

static void test_json_skip_space(JsonCursor *cursor)
{
    while (*cursor->current == (unsigned char)' ' ||
           *cursor->current == (unsigned char)'\t' ||
           *cursor->current == (unsigned char)'\r' ||
           *cursor->current == (unsigned char)'\n') {
        cursor->current++;
    }
}

static int test_json_string(JsonCursor *cursor)
{
    if (*cursor->current++ != (unsigned char)'"') return 0;
    while (*cursor->current != 0U) {
        unsigned char byte = *cursor->current++;
        if (byte == (unsigned char)'"') return 1;
        if (byte < 0x20U) return 0;
        if (byte == (unsigned char)'\\') {
            unsigned char escaped = *cursor->current++;
            if (escaped == (unsigned char)'u') {
                unsigned index;
                for (index = 0U; index < 4U; ++index) {
                    if (!isxdigit((int)*cursor->current)) return 0;
                    cursor->current++;
                }
            } else if (strchr("\"\\/bfnrt", (int)escaped) == NULL) {
                return 0;
            }
        }
    }
    return 0;
}

static int test_json_value(JsonCursor *cursor);

static int test_json_array(JsonCursor *cursor)
{
    cursor->current++;
    test_json_skip_space(cursor);
    if (*cursor->current == (unsigned char)']') {
        cursor->current++;
        return 1;
    }
    for (;;) {
        if (!test_json_value(cursor)) return 0;
        test_json_skip_space(cursor);
        if (*cursor->current == (unsigned char)']') {
            cursor->current++;
            return 1;
        }
        if (*cursor->current++ != (unsigned char)',') return 0;
        test_json_skip_space(cursor);
    }
}

static int test_json_object(JsonCursor *cursor)
{
    cursor->current++;
    test_json_skip_space(cursor);
    if (*cursor->current == (unsigned char)'}') {
        cursor->current++;
        return 1;
    }
    for (;;) {
        if (!test_json_string(cursor)) return 0;
        test_json_skip_space(cursor);
        if (*cursor->current++ != (unsigned char)':') return 0;
        test_json_skip_space(cursor);
        if (!test_json_value(cursor)) return 0;
        test_json_skip_space(cursor);
        if (*cursor->current == (unsigned char)'}') {
            cursor->current++;
            return 1;
        }
        if (*cursor->current++ != (unsigned char)',') return 0;
        test_json_skip_space(cursor);
    }
}

static int test_json_number(JsonCursor *cursor)
{
    const unsigned char *start = cursor->current;
    if (*cursor->current == (unsigned char)'-') cursor->current++;
    if (*cursor->current == (unsigned char)'0') {
        cursor->current++;
    } else if (*cursor->current >= (unsigned char)'1' &&
               *cursor->current <= (unsigned char)'9') {
        do {
            cursor->current++;
        } while (isdigit((int)*cursor->current));
    } else {
        return 0;
    }
    if (*cursor->current == (unsigned char)'.') {
        cursor->current++;
        if (!isdigit((int)*cursor->current)) return 0;
        while (isdigit((int)*cursor->current)) cursor->current++;
    }
    if (*cursor->current == (unsigned char)'e' ||
        *cursor->current == (unsigned char)'E') {
        cursor->current++;
        if (*cursor->current == (unsigned char)'+' ||
            *cursor->current == (unsigned char)'-') {
            cursor->current++;
        }
        if (!isdigit((int)*cursor->current)) return 0;
        while (isdigit((int)*cursor->current)) cursor->current++;
    }
    return cursor->current != start;
}

static int test_json_value(JsonCursor *cursor)
{
    int result;
    test_json_skip_space(cursor);
    if (cursor->depth++ > 64U) return 0;
    if (*cursor->current == (unsigned char)'{') {
        result = test_json_object(cursor);
    } else if (*cursor->current == (unsigned char)'[') {
        result = test_json_array(cursor);
    } else if (*cursor->current == (unsigned char)'"') {
        result = test_json_string(cursor);
    } else if (strncmp((const char *)cursor->current, "true", 4U) == 0) {
        cursor->current += 4U;
        result = 1;
    } else if (strncmp((const char *)cursor->current, "false", 5U) == 0) {
        cursor->current += 5U;
        result = 1;
    } else if (strncmp((const char *)cursor->current, "null", 4U) == 0) {
        cursor->current += 4U;
        result = 1;
    } else {
        result = test_json_number(cursor);
    }
    cursor->depth--;
    return result;
}

static int test_json_valid(const char *text)
{
    JsonCursor cursor;
    if (text == NULL) return 0;
    cursor.current = (const unsigned char *)text;
    cursor.depth = 0U;
    if (!test_json_value(&cursor)) return 0;
    test_json_skip_space(&cursor);
    return *cursor.current == 0U;
}

static int test_contains(const char *path, const char *needle)
{
    size_t size;
    unsigned char *data = test_read_file(path, &size);
    int found = data != NULL && strstr((const char *)data, needle) != NULL;
    (void)size;
    free(data);
    return found;
}

static int test_next_item_record(FILE *stream, char *line, size_t size)
{
    while (fgets(line, (int)size, stream) != NULL) {
        if (strncmp(line, "EVENT,", 6U) == 0 ||
            strncmp(line, "ITEM,", 5U) == 0 ||
            strncmp(line, "MEMBER,", 7U) == 0 ||
            strncmp(line, "WARNING,", 8U) == 0) {
            return 1;
        }
    }
    return ferror(stream) ? -1 : 0;
}

static int test_item_records_equal(const char *left_path,
                                   const char *right_path)
{
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    char left_line[8192];
    char right_line[8192];
    int equal = left != NULL && right != NULL;

    while (equal) {
        int left_result = test_next_item_record(
            left, left_line, sizeof(left_line));
        int right_result = test_next_item_record(
            right, right_line, sizeof(right_line));
        if (left_result != right_result || left_result < 0) {
            equal = 0;
        } else if (left_result == 0) {
            break;
        } else if (strcmp(left_line, right_line) != 0) {
            equal = 0;
        }
    }
    if (left != NULL && fclose(left) != 0) equal = 0;
    if (right != NULL && fclose(right) != 0) equal = 0;
    return equal;
}

static char *test_fourth_last_comma(char *start, char *end)
{
    char *cursor = end;
    unsigned count = 0U;

    while (cursor > start) {
        cursor--;
        if (*cursor == ',') {
            count++;
            if (count == 4U) return cursor;
        }
    }
    return NULL;
}

static int test_write_exclusion_edit(const char *source,
                                     const char *target,
                                     int excluded)
{
    FILE *input = fopen(source, "rb");
    FILE *output = NULL;
    char line[65536];
    int changed_meta = 0;
    int changed_item = 0;
    int okay = input != NULL;

    if (okay) {
        output = fopen(target, "wb");
        okay = output != NULL;
    }
    while (okay && fgets(line, (int)sizeof(line), input) != NULL) {
        if (strncmp(line, "META,excluded_item_count,", 25U) == 0) {
            if (fprintf(output, "META,excluded_item_count,%d,items\r\n",
                        excluded != 0 ? 1 : 0) < 0) {
                okay = 0;
            }
            changed_meta = 1;
        } else if (!changed_item && strncmp(line, "ITEM,", 5U) == 0) {
            char *line_end = strstr(line, "\r\n");
            char *origin = line_end == NULL
                               ? NULL
                               : test_fourth_last_comma(line, line_end);
            size_t prefix_size = origin == NULL
                                     ? 0U
                                     : (size_t)(origin + 1 - line);
            if (origin == NULL ||
                fwrite(line, 1U, prefix_size, output) != prefix_size ||
                fprintf(output, "manual,0,%d,%s\r\n",
                        excluded != 0 ? 1 : 0,
                        excluded != 0 ? "manual reject" : "") < 0) {
                okay = 0;
            }
            changed_item = 1;
        } else if (fputs(line, output) == EOF) {
            okay = 0;
        }
    }
    if (input != NULL && ferror(input)) okay = 0;
    if (output != NULL && fclose(output) != 0) okay = 0;
    if (input != NULL && fclose(input) != 0) okay = 0;
    okay = okay && changed_meta && changed_item;
    if (!okay) (void)test_remove_file(target);
    return okay;
}

static int test_make_score_alignment(TestFiles *files)
{
    const char *arguments[] = {
        "--frame-size", "512", "--hop-size", "128",
        "--alignment-step", "0.05", "--coarse-step", "0.20",
        "--dtw-band", "2", "--fine-radius", "0.40",
        "--score", files->score, "align", files->audio,
        "--output", files->alignment
    };
    return test_run(files, arguments,
                    sizeof(arguments) / sizeof(arguments[0]));
}

static int test_make_second_score_alignment(TestFiles *files)
{
    const char *arguments[] = {
        "--frame-size", "512", "--hop-size", "128",
        "--alignment-step", "0.05", "--coarse-step", "0.20",
        "--dtw-band", "2", "--fine-radius", "0.40",
        "--match-threshold", "0.20",
        "--score", files->score, "align", files->audio,
        "--output", files->second_alignment
    };
    return test_run(files, arguments,
                    sizeof(arguments) / sizeof(arguments[0]));
}

static int test_make_audio_alignment(TestFiles *files)
{
    const char *arguments[] = {
        "--frame-size", "512", "--hop-size", "128",
        "align", files->audio, files->changed_audio,
        "--output", files->audio_alignment
    };
    return test_run(files, arguments,
                    sizeof(arguments) / sizeof(arguments[0]));
}

static int test_basic(void)
{
    TestFiles files;
    const char *arguments[9];
    size_t count = 0U;

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0,
          "could not make score alignment");
    arguments[count++] = "--alignment"; arguments[count++] = files.alignment;
    arguments[count++] = "--labels"; arguments[count++] = files.labels;
    arguments[count++] = "segment"; arguments[count++] = files.audio;
    arguments[count++] = "--output"; arguments[count++] = files.items;
    CHECK(test_run(&files, arguments, count) == 0,
          "basic segment command failed");
    CHECK(test_contains(files.items, "HWA_ITEMS,1"),
          "item file has no schema magic");
    CHECK(test_contains(files.items, "EVENT,"),
          "item file has no source events");
    CHECK(test_contains(files.items, "ITEM,"),
          "item file has no items");
    CHECK(test_contains(files.items, "MEMBER,"),
          "item file has no memberships");
    CHECK(test_contains(files.items, "undeclared-element") == 0,
          "generic fixture gained an undeclared label");
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_json_and_labels(void)
{
    TestFiles files;
    const char *arguments[10];
    size_t count = 0U;
    size_t json_size;
    unsigned char *json;

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0,
          "could not make score alignment");
    arguments[count++] = "--json";
    arguments[count++] = "--alignment"; arguments[count++] = files.alignment;
    arguments[count++] = "--labels"; arguments[count++] = files.labels;
    arguments[count++] = "segment"; arguments[count++] = files.audio;
    arguments[count++] = "--output"; arguments[count++] = files.items;
    CHECK(test_run(&files, arguments, count) == 0,
          "JSON segment command failed");
    json = test_read_file(files.output, &json_size);
    CHECK(json != NULL && test_json_valid((const char *)json),
          "segment summary is not valid JSON");
    CHECK(json != NULL &&
              strstr((const char *)json, "\"schema_version\":4") != NULL,
          "JSON summary has no schema 4");
    CHECK(json != NULL &&
              strstr((const char *)json, "\"command\":\"segment\"") != NULL,
          "JSON summary has no segment command");
    CHECK(test_contains(files.items, "resonator-a") &&
              test_contains(files.items, "blend") &&
              test_contains(files.items, "impulse"),
          "typed labels did not reach the item file");
    (void)json_size;
    free(json);
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_rejections(void)
{
    TestFiles files;
    const char *audio_mode[] = {
        "--alignment", files.audio_alignment, "segment", files.audio,
        "--output", files.items
    };
    const char *wrong_audio[] = {
        "--alignment", files.alignment, "segment", files.changed_audio,
        "--output", files.items
    };
    const char *unknown_label[] = {
        "--alignment", files.alignment, "--labels", files.bad_labels,
        "segment", files.audio, "--output", files.items
    };
    const char *clock_override[] = {
        "--frame-size", "1024", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *hop_override[] = {
        "--hop-size", "64", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *silence_override[] = {
        "--silence-threshold", "-50", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *mix_override[] = {
        "--mixdown", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0 &&
              test_make_audio_alignment(&files) == 0,
          "could not make rejection alignments");
    CHECK(test_run(&files, audio_mode,
                   sizeof(audio_mode) / sizeof(audio_mode[0])) != 0,
          "segment accepted an audio-to-audio alignment");
    CHECK(test_run(&files, wrong_audio,
                   sizeof(wrong_audio) / sizeof(wrong_audio[0])) != 0,
          "segment accepted an audio hash mismatch");
    CHECK(test_run(&files, unknown_label,
                   sizeof(unknown_label) / sizeof(unknown_label[0])) != 0,
          "segment accepted an unknown label event");
    CHECK(test_run(&files, clock_override,
                   sizeof(clock_override) / sizeof(clock_override[0])) == 2,
          "segment accepted a feature-clock override");
    CHECK(test_run(&files, hop_override,
                   sizeof(hop_override) / sizeof(hop_override[0])) == 2,
          "segment accepted a hop-size override");
    CHECK(test_run(&files, silence_override,
                   sizeof(silence_override) / sizeof(silence_override[0])) == 2,
          "segment accepted an activity-threshold override");
    CHECK(test_run(&files, mix_override,
                   sizeof(mix_override) / sizeof(mix_override[0])) == 2,
          "segment accepted a channel-mode override");
    CHECK(!test_path_exists(files.items),
          "rejected segment command published an item file");
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_limits_and_output(void)
{
    TestFiles files;
    char other_items[PATH_MAX];
    const char *limit[] = {
        "--max-events", "1", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *audio_output[] = {
        "--replace", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.audio
    };
    const char *alignment_output[] = {
        "--replace", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.alignment
    };
    const char *hardlink_output[] = {
        "--replace", "--alignment", files.alignment,
        "--labels", files.labels, "segment", files.audio,
        "--output", other_items
    };
    const char *successful[] = {
        "--alignment", files.alignment, "--labels", files.labels,
        "segment", files.audio, "--output", files.items
    };
#if !defined(_WIN32)
    const char *broken_new[] = {
        "--alignment", files.alignment, "segment", files.audio,
        "--output", files.items
    };
    const char *broken_json[] = {
        "--json", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *broken_replace[] = {
        "--replace", "--block-frames", "1",
        "--alignment", files.alignment, "segment", files.audio,
        "--output", files.items
    };
    size_t item_size;
    unsigned char *item_before;
#endif
    size_t audio_size;
    size_t alignment_size;
    size_t labels_size;
    unsigned char *audio_before;
    unsigned char *alignment_before;
    unsigned char *labels_before;

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0,
          "could not make score alignment");
    CHECK(test_join(other_items, files.directory, "other.hwa-items"),
          "hard-link path is too long");
    audio_before = test_read_file(files.audio, &audio_size);
    alignment_before = test_read_file(files.alignment, &alignment_size);
    labels_before = test_read_file(files.labels, &labels_size);
    CHECK(audio_before != NULL && alignment_before != NULL &&
              labels_before != NULL,
          "could not snapshot segment inputs");
    CHECK(test_run(&files, limit, sizeof(limit) / sizeof(limit[0])) != 0,
          "segment ignored max-events");
    CHECK(!test_path_exists(files.items),
          "failed event cap published an item file");
    CHECK(test_run(&files, audio_output,
                   sizeof(audio_output) / sizeof(audio_output[0])) != 0,
          "segment replaced its audio input");
    CHECK(test_run(&files, alignment_output,
                   sizeof(alignment_output) / sizeof(alignment_output[0])) != 0,
          "segment replaced its alignment input");
    CHECK(test_make_hard_link(files.labels, other_items),
          "could not make a labels hard link");
    CHECK(test_run(&files, hardlink_output,
                   sizeof(hardlink_output) / sizeof(hardlink_output[0])) != 0,
          "segment replaced a hard link to its labels input");
    (void)test_remove_file(other_items);
    CHECK(test_run(&files, successful,
                   sizeof(successful) / sizeof(successful[0])) == 0,
          "could not run the input-integrity segment command");
    CHECK(test_file_matches(files.audio, audio_before, audio_size),
          "segment changed its audio input bytes");
    CHECK(test_file_matches(files.alignment, alignment_before, alignment_size),
          "segment changed its alignment input bytes");
    CHECK(test_file_matches(files.labels, labels_before, labels_size),
          "segment changed its labels input bytes");
    (void)test_remove_file(files.items);
#if !defined(_WIN32)
    CHECK(test_run_with_broken_stdout(
              &files, broken_new,
              sizeof(broken_new) / sizeof(broken_new[0])) == 1,
          "broken standard output did not fail a named create");
    CHECK(test_contains(files.error, "cannot write standard output"),
          "broken standard output had no clear error");
    CHECK(!test_path_exists(files.items),
          "broken standard output published a new item file");
    CHECK(test_run_with_broken_stdout(
              &files, broken_json,
              sizeof(broken_json) / sizeof(broken_json[0])) == 1,
          "broken JSON output did not fail a named create");
    CHECK(test_contains(files.error, "cannot write standard output"),
          "broken JSON output had no clear error");
    CHECK(!test_path_exists(files.items),
          "broken JSON output published a new item file");
    CHECK(test_run(&files, broken_new,
                   sizeof(broken_new) / sizeof(broken_new[0])) == 0,
          "could not make the replacement baseline");
    item_before = test_read_file(files.items, &item_size);
    CHECK(item_before != NULL, "could not snapshot the replacement baseline");
    CHECK(test_run_with_broken_stdout(
              &files, broken_replace,
              sizeof(broken_replace) / sizeof(broken_replace[0])) == 1,
          "broken standard output did not fail a named replacement");
    CHECK(test_contains(files.error, "cannot write standard output"),
          "broken replacement had no clear standard-output error");
    CHECK(test_file_matches(files.items, item_before, item_size),
          "broken standard output replaced an item file");
    free(item_before);
#endif
    free(labels_before);
    free(alignment_before);
    free(audio_before);
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_amend_and_provenance(void)
{
    TestFiles files;
    char other_items[PATH_MAX];
    const char *make_items[] = {
        "--alignment", files.alignment, "--labels", files.labels,
        "segment", files.audio, "--output", files.items
    };
    const char *amend[] = {
        "--alignment", files.alignment, "--labels", files.labels,
        "--amend", files.items, "segment", files.audio,
        "--output", files.second_items
    };
    const char *stale_alignment[] = {
        "--alignment", files.second_alignment, "--labels", files.labels,
        "--amend", files.items, "segment", files.audio,
        "--output", files.second_items
    };
    const char *stale_labels[] = {
        "--alignment", files.alignment, "--labels", files.changed_labels,
        "--amend", files.items, "segment", files.audio,
        "--output", files.second_items
    };
    const char *stale_audio[] = {
        "--alignment", files.alignment, "--labels", files.labels,
        "--amend", files.items, "segment", files.changed_audio,
        "--output", files.second_items
    };
    const char *amend_alias[] = {
        "--replace", "--alignment", files.alignment,
        "--labels", files.labels, "--amend", files.items,
        "segment", files.audio, "--output", files.items
    };
    const char *exclude[] = {
        "--alignment", files.alignment, "--labels", files.labels,
        "--amend", files.second_items, "segment", files.audio,
        "--output", other_items
    };
    const char *clear_exclusion[] = {
        "--replace", "--alignment", files.alignment,
        "--labels", files.labels, "--amend", files.second_items,
        "segment", files.audio, "--output", files.items
    };
    size_t before_size;
    size_t after_size;
    unsigned char *before;
    unsigned char *after;

    if (!test_setup(&files)) return 1;
    CHECK(test_join(other_items, files.directory, "other.hwa-items"),
          "amendment output path is too long");
    CHECK(test_make_score_alignment(&files) == 0 &&
              test_make_second_score_alignment(&files) == 0,
          "could not make amendment alignments");
    CHECK(test_run(&files, make_items,
                   sizeof(make_items) / sizeof(make_items[0])) == 0,
          "could not make base items");
    CHECK(test_run(&files, amend, sizeof(amend) / sizeof(amend[0])) == 0,
          "unchanged amendment failed");
    CHECK(test_contains(files.second_items, "amendment"),
          "amended output has no amendment provenance");
    (void)test_remove_file(files.second_items);
    CHECK(test_run(&files, stale_alignment,
                   sizeof(stale_alignment) / sizeof(stale_alignment[0])) != 0,
          "amendment accepted a stale alignment hash");
    CHECK(test_run(&files, stale_labels,
                   sizeof(stale_labels) / sizeof(stale_labels[0])) != 0,
          "amendment accepted stale typed labels");
    CHECK(test_run(&files, stale_audio,
                   sizeof(stale_audio) / sizeof(stale_audio[0])) != 0,
          "amendment accepted a stale audio file");
    CHECK(!test_path_exists(files.second_items),
          "rejected amendment published an item file");
    before = test_read_file(files.items, &before_size);
    CHECK(before != NULL, "could not snapshot amendment");
    CHECK(test_run(&files, amend_alias,
                   sizeof(amend_alias) / sizeof(amend_alias[0])) != 0,
          "segment replaced its amendment input");
    after = test_read_file(files.items, &after_size);
    CHECK(after != NULL && before_size == after_size &&
              memcmp(before, after, before_size) == 0,
          "failed amendment alias changed its bytes");
    free(after);
    free(before);
    CHECK(test_write_exclusion_edit(files.items, files.second_items, 1),
          "could not make an exclusion-only amendment");
    before = test_read_file(files.second_items, &before_size);
    CHECK(before != NULL, "could not snapshot exclusion amendment");
    CHECK(test_run(&files, exclude, sizeof(exclude) / sizeof(exclude[0])) == 0,
          "named exclusion-only amendment failed");
    CHECK(test_file_matches(files.second_items, before, before_size),
          "segment changed its exclusion amendment input");
    CHECK(test_contains(other_items, "META,excluded_item_count,1,items\r\n") &&
              test_contains(other_items,
                            ",manual,0,1,manual reject\r\n"),
          "named amendment lost the exclusion-only edit");
    free(before);
    CHECK(test_write_exclusion_edit(other_items, files.second_items, 0),
          "could not make a clear-exclusion amendment");
    before = test_read_file(files.second_items, &before_size);
    CHECK(before != NULL, "could not snapshot clear-exclusion amendment");
    CHECK(test_run(&files, clear_exclusion,
                   sizeof(clear_exclusion) / sizeof(clear_exclusion[0])) == 0,
          "named clear-exclusion amendment failed");
    CHECK(test_file_matches(files.second_items, before, before_size),
          "segment changed its clear-exclusion amendment input");
    CHECK(test_contains(files.items,
                        "META,excluded_item_count,0,items\r\n") &&
              test_contains(files.items, ",manual,0,0,\r\n"),
          "named amendment did not clear the prior exclusion");
    free(before);
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_output_modes_and_blocks(void)
{
    TestFiles files;
    const char *standard_output[] = {
        "--alignment", files.alignment, "segment", files.audio,
        "--output", "-"
    };
    const char *json_conflict[] = {
        "--json", "--alignment", files.alignment, "segment", files.audio,
        "--output", "-"
    };
    const char *replace_conflict[] = {
        "--replace", "--alignment", files.alignment, "segment", files.audio,
        "--output", "-"
    };
    const char *named[] = {
        "--alignment", files.alignment, "segment", files.audio,
        "--output", files.items
    };
    const char *replace[] = {
        "--replace", "--alignment", files.alignment, "segment", files.audio,
        "--output", files.items
    };
    const char *block_one[] = {
        "--block-frames", "1", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.items
    };
    const char *block_large[] = {
        "--block-frames", "4096", "--alignment", files.alignment,
        "segment", files.audio, "--output", files.second_items
    };

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0,
          "could not make score alignment");
    CHECK(test_run(&files, standard_output,
                   sizeof(standard_output) / sizeof(standard_output[0])) == 0 &&
              test_contains(files.output, "HWA_ITEMS,1"),
          "canonical standard output failed");
    CHECK(test_run(&files, json_conflict,
                   sizeof(json_conflict) / sizeof(json_conflict[0])) == 2,
          "--json did not conflict with canonical standard output");
    CHECK(test_run(&files, replace_conflict,
                   sizeof(replace_conflict) / sizeof(replace_conflict[0])) == 2,
          "--replace did not conflict with standard output");
    CHECK(test_run(&files, named, sizeof(named) / sizeof(named[0])) == 0,
          "exclusive item create failed");
    CHECK(test_run(&files, named, sizeof(named) / sizeof(named[0])) != 0,
          "second exclusive create replaced an item file");
    CHECK(test_run(&files, replace, sizeof(replace) / sizeof(replace[0])) == 0,
          "explicit item replacement failed");
    (void)test_remove_file(files.items);
    CHECK(test_run(&files, block_one,
                   sizeof(block_one) / sizeof(block_one[0])) == 0 &&
              test_run(&files, block_large,
                       sizeof(block_large) / sizeof(block_large[0])) == 0,
          "decode-block segmentation run failed");
    CHECK(test_item_records_equal(files.items, files.second_items),
          "item records changed with the decode block split");
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

static int test_explicit_audio_authority(void)
{
    TestFiles files;
    const char *arguments[] = {
        "--alignment", files.alignment, "segment", files.audio_copy,
        "--output", files.items
    };

    if (!test_setup(&files)) return 1;
    CHECK(test_make_score_alignment(&files) == 0,
          "could not make score alignment");
    CHECK(test_remove_file(files.score) == 0 &&
              test_remove_file(files.audio) == 0,
          "could not remove stored alignment paths");
    CHECK(test_run(&files, arguments,
                   sizeof(arguments) / sizeof(arguments[0])) == 0,
          "segment opened a stored path instead of the explicit audio copy");
    CHECK(test_contains(files.items, "617564696f2d636f70792e776176"),
          "item provenance did not keep the explicit audio path");
    test_files_close(&files);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *case_name;

    if (argc != 3) {
        (void)fputs("usage: stage3_cli_tests ANALYZER CASE\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    case_name = argv[2];
    if (strcmp(case_name, "stage3-basic") == 0) return test_basic();
    if (strcmp(case_name, "stage3-json-labels") == 0) {
        return test_json_and_labels();
    }
    if (strcmp(case_name, "stage3-rejections") == 0) return test_rejections();
    if (strcmp(case_name, "stage3-limits-output") == 0) {
        return test_limits_and_output();
    }
    if (strcmp(case_name, "stage3-amend") == 0) {
        return test_amend_and_provenance();
    }
    if (strcmp(case_name, "stage3-output-modes-blocks") == 0) {
        return test_output_modes_and_blocks();
    }
    if (strcmp(case_name, "stage3-explicit-audio") == 0) {
        return test_explicit_audio_authority();
    }
    (void)fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
