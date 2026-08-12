#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "item_file.h"
#include "sha256.h"

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
#define TEST_FRAMES 8000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestFiles {
    char directory[PATH_MAX];
    char audio[PATH_MAX];
    char audio_alias[PATH_MAX];
    char items[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    char third[PATH_MAX];
    char comparison[PATH_MAX];
    char output[PATH_MAX];
    char error[PATH_MAX];
} TestFiles;

static const char *analyzer_path;
static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
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
                              "%s/hwa-stage4-cli-%ld-%u", root,
                              test_process_id(), attempt);
        if (length < 0 || (size_t)length >= sizeof(files->directory)) return 0;
        if (test_make_directory(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt != 100U &&
           test_join(files->audio, files->directory, "audio.wav") &&
           test_join(files->audio_alias, files->directory, "audio-alias.wav") &&
           test_join(files->items, files->directory, "audio.hwa-items") &&
           test_join(files->first, files->directory, "first.hwa-measures") &&
           test_join(files->second, files->directory, "second.hwa-measures") &&
           test_join(files->third, files->directory, "third.hwa-measures") &&
           test_join(files->comparison, files->directory,
                     "result.hwa-compare") &&
           test_join(files->output, files->directory, "stdout.txt") &&
           test_join(files->error, files->directory, "stderr.txt");
}

static void test_files_close(TestFiles *files)
{
    const char *paths[] = {
        files->audio, files->audio_alias, files->items, files->first,
        files->second, files->third, files->comparison, files->output,
        files->error
    };
    size_t index;
    for (index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        (void)test_remove_file(paths[index]);
    }
    (void)test_remove_directory(files->directory);
}

static int test_write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_wav(const char *path)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + TEST_FRAMES * 2U) &&
           test_write_bytes(stream, "WAVE", 4U) &&
           test_write_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
           test_write_u16(stream, 1U) && test_write_u32(stream, TEST_RATE) &&
           test_write_u32(stream, TEST_RATE * 2U) &&
           test_write_u16(stream, 2U) && test_write_u16(stream, 16U) &&
           test_write_bytes(stream, "data", 4U) &&
           test_write_u32(stream, TEST_FRAMES * 2U);
    for (frame = 0U; okay && frame < TEST_FRAMES; ++frame) {
        double time = (double)frame / (double)TEST_RATE;
        double envelope = time < 0.05 ? time / 0.05 :
                          time > 0.95 ? (1.0 - time) / 0.05 : 1.0;
        double phase = 2.0 * TEST_PI * 440.0 * time;
        double value = 0.2 * envelope *
                       (sin(phase) + 0.35 * sin(2.0 * phase));
        int16_t sample = (int16_t)lrint(value * 32767.0);
        okay = test_write_u16(stream, (uint16_t)sample);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static void test_hash(char target[HWA_SHA256_HEX_SIZE], char byte)
{
    size_t index;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        target[index] = byte;
    }
    target[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static int test_write_items(const TestFiles *files)
{
    HWAItemSet set;
    HWAItemEvent event;
    HWAItem items[2];
    HWAItemMember members[2];
    FILE *stream;
    char error[HWA_ERROR_SIZE];
    memset(&set, 0, sizeof(set));
    memset(&event, 0, sizeof(event));
    memset(items, 0, sizeof(items));
    memset(members, 0, sizeof(members));
    hwa_segmentation_options_default(&set.options);
    set.alignment_path = (char *)"fixture.hwa-align";
    set.audio_path = (char *)files->audio;
    set.source_score_path = (char *)"score.csv";
    test_hash(set.alignment_sha256, 'a');
    test_hash(set.source_score_sha256, 'b');
    if (hwa_sha256_file(files->audio, UINT64_C(1048576), set.audio_sha256,
                        error, sizeof(error)) != 0) return 0;
    set.audio_format.sample_rate_hz = TEST_RATE;
    set.audio_format.frames = TEST_FRAMES;
    set.audio_format.duration_seconds = 1.0;
    set.source_score_duration_seconds = 1.0;
    set.alignment_confidence = 1.0;
    set.events = &event;
    set.event_count = 1U;
    event.id = 1U;
    event.event_id = (char *)"n1";
    event.kind = (char *)"note";
    event.voice = (char *)"solo";
    event.midi_note = (char *)"69";
    event.dynamic = (char *)"mf";
    event.score_position = (char *)"test";
    event.labels.pitch = (char *)"A4";
    event.labels.register_name = (char *)"octave-4";
    event.labels.dynamic = (char *)"mf";
    event.labels.part = (char *)"solo";
    event.labels.physical_element = (char *)"resonator-a";
    event.labels.controller = (char *)"exciter";
    event.labels.score_section = (char *)"test";
    event.score_end_beat = 2.0;
    event.score_end_seconds = 1.0;
    event.audio_end_sample = TEST_FRAMES;
    event.audio_end_seconds = 1.0;
    event.tempo_bpm = 120.0;
    event.alignment_confidence = 1.0;
    event.alignment_status = HWA_ALIGNMENT_MATCHED;
    event.tempo_valid = 1;
    set.items = items;
    set.item_count = 2U;
    items[0].id = 1U;
    items[0].key = (char *)"note:1";
    items[0].role = (char *)"note";
    items[0].kind = HWA_ITEM_NOTE;
    items[0].end_sample = TEST_FRAMES;
    items[0].end_seconds = 1.0;
    items[0].score_end_beat = 2.0;
    items[0].confidence = 1.0;
    items[0].origin = HWA_ITEM_ORIGIN_AUTO;
    items[1] = items[0];
    items[1].id = 2U;
    items[1].key = (char *)"body:1";
    items[1].role = (char *)"body";
    items[1].kind = HWA_ITEM_BODY;
    items[1].parent_id = 1U;
    items[1].parent_valid = 1;
    items[1].start_sample = 400U;
    items[1].end_sample = 7600U;
    items[1].start_seconds = 0.05;
    items[1].end_seconds = 0.95;
    set.members = members;
    set.member_count = 2U;
    members[0].item_id = 1U;
    members[0].event_id = 1U;
    members[0].role = HWA_ITEM_MEMBER_SOURCE;
    members[1].item_id = 2U;
    members[1].event_id = 1U;
    members[1].role = HWA_ITEM_MEMBER_SOURCE;
    stream = fopen(files->items, "wb");
    if (stream == NULL) return 0;
    if (hwa_item_file_write(stream, &set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "item fixture write failed: %s\n", error);
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

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

static int test_run(const TestFiles *files,
                    const char *const *arguments,
                    size_t argument_count)
{
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
#if defined(_WIN32)
    return status;
#else
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
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(output_pipe[1]);
        error_file = open(files->error, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (error_file < 0 || dup2(error_file, STDERR_FILENO) < 0) _exit(126);
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
    okay = (end == 0L ||
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

static int test_contains(const char *path, const char *needle)
{
    size_t size;
    unsigned char *bytes = test_read_file(path, &size);
    int found = bytes != NULL && strstr((const char *)bytes, needle) != NULL;
    (void)size;
    free(bytes);
    return found;
}

static int test_file_matches(const char *path,
                             const unsigned char *expected,
                             size_t expected_size)
{
    size_t size;
    unsigned char *bytes = test_read_file(path, &size);
    int match = bytes != NULL && size == expected_size &&
                memcmp(bytes, expected, size) == 0;
    free(bytes);
    return match;
}

static int test_next_measure_row(FILE *stream, char *line, size_t size)
{
    while (fgets(line, (int)size, stream) != NULL) {
        if (strncmp(line, "CONTEXT,", 8U) == 0 ||
            strncmp(line, "MEASURE,", 8U) == 0 ||
            strncmp(line, "GROUP,", 6U) == 0 ||
            strncmp(line, "GROUP_MEMBER,", 13U) == 0 ||
            strncmp(line, "STAT,", 5U) == 0 ||
            strncmp(line, "WARNING,", 8U) == 0) return 1;
    }
    return ferror(stream) ? -1 : 0;
}

static int test_measure_rows_equal(const char *left_path,
                                   const char *right_path)
{
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    char left_line[8192];
    char right_line[8192];
    int equal = left != NULL && right != NULL;
    while (equal) {
        int left_result = test_next_measure_row(
            left, left_line, sizeof(left_line));
        int right_result = test_next_measure_row(
            right, right_line, sizeof(right_line));
        if (left_result != right_result || left_result < 0) equal = 0;
        else if (left_result == 0) break;
        else if (strcmp(left_line, right_line) != 0) equal = 0;
    }
    if (left != NULL && fclose(left) != 0) equal = 0;
    if (right != NULL && fclose(right) != 0) equal = 0;
    return equal;
}

static void test_measure_and_compare_cli(void)
{
    TestFiles files;
    char audio_hash[HWA_SHA256_HEX_SIZE];
    char items_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *audio_before;
    unsigned char *items_before;
    size_t audio_size;
    size_t items_size;
    const char *block_one[] = {
        "--block-frames", "1", "--measure-fft-size", "512",
        "--measure-hop-size", "64", "--items", files.items,
        "measure", files.audio, "--output", files.first
    };
    const char *block_large[] = {
        "--block-frames", "4096", "--measure-fft-size", "512",
        "--measure-hop-size", "64", "--items", files.items,
        "measure", files.audio, "--output", files.second
    };
    const char *exclusive[] = {
        "--measure-fft-size", "512", "--measure-hop-size", "64",
        "--items", files.items, "measure", files.audio,
        "--output", files.first
    };
    const char *replace[] = {
        "--replace", "--block-frames", "1", "--measure-fft-size", "512",
        "--measure-hop-size", "64", "--items", files.items,
        "measure", files.audio, "--output", files.first
    };
    const char *audio_alias[] = {
        "--replace", "--measure-fft-size", "512",
        "--measure-hop-size", "64", "--items", files.items,
        "measure", files.audio, "--output", files.audio_alias
    };
    const char *canonical_stdout[] = {
        "--measure-fft-size", "512", "--measure-hop-size", "64",
        "--items", files.items, "measure", files.audio, "--output", "-"
    };
    const char *broken[] = {
        "--measure-fft-size", "512", "--measure-hop-size", "64",
        "--items", files.items, "measure", files.audio,
        "--output", files.third
    };
    const char *compare[] = {
        "compare-measures", files.first, files.second,
        "--output", files.comparison
    };
    const char *compare_stdout[] = {
        "compare-measures", files.first, files.second, "--output", "-"
    };
    const char *compare_cap[] = {
        "--max-comparison-work-bytes", "1", "compare-measures",
        files.first, files.second, "--output", files.third
    };
    const char *compare_alias[] = {
        "--replace", "compare-measures", files.first, files.second,
        "--output", files.first
    };
    const char *compare_json[] = {
        "--replace", "--json", "compare-measures", files.first,
        files.second, "--output", files.comparison
    };

    CHECK(test_files_open(&files), "cannot make CLI workspace");
    if (failures != 0) return;
    CHECK(test_write_wav(files.audio) && test_write_items(&files),
          "cannot make CLI fixtures");
    audio_before = test_read_file(files.audio, &audio_size);
    items_before = test_read_file(files.items, &items_size);
    CHECK(audio_before != NULL && items_before != NULL,
          "cannot snapshot measure inputs");
    CHECK(hwa_sha256_file(files.audio, UINT64_MAX, audio_hash,
                          error, sizeof(error)) == 0 &&
              hwa_sha256_file(files.items, UINT64_MAX, items_hash,
                              error, sizeof(error)) == 0,
          "cannot hash measure inputs");
    CHECK(test_run(&files, block_one,
                   sizeof(block_one) / sizeof(block_one[0])) == 0 &&
              test_run(&files, block_large,
                       sizeof(block_large) / sizeof(block_large[0])) == 0,
          "measure decode-block runs failed");
    CHECK(test_measure_rows_equal(files.first, files.second),
          "measure rows changed with the decode block split");
    CHECK(test_contains(files.first, audio_hash) &&
              test_contains(files.first, items_hash),
          "measurement profile lost input hashes");
    CHECK(test_file_matches(files.audio, audio_before, audio_size) &&
              test_file_matches(files.items, items_before, items_size),
          "measure changed an input");
    CHECK(test_run(&files, exclusive,
                   sizeof(exclusive) / sizeof(exclusive[0])) != 0,
          "exclusive measurement create replaced a profile");
    CHECK(test_run(&files, replace, sizeof(replace) / sizeof(replace[0])) == 0,
          "explicit measurement replacement failed");
    CHECK(test_make_hard_link(files.audio, files.audio_alias),
          "cannot make audio output alias");
    CHECK(test_run(&files, audio_alias,
                   sizeof(audio_alias) / sizeof(audio_alias[0])) != 0 &&
              test_file_matches(files.audio, audio_before, audio_size),
          "measure wrote through an alias to its audio input");
    (void)test_remove_file(files.audio_alias);
    CHECK(test_run(&files, canonical_stdout,
                   sizeof(canonical_stdout) /
                       sizeof(canonical_stdout[0])) == 0 &&
              test_contains(files.output, "HWA_MEASURES,1"),
          "canonical measurement standard output failed");
#if !defined(_WIN32)
    CHECK(test_run_with_broken_stdout(
              &files, broken, sizeof(broken) / sizeof(broken[0])) == 1,
          "broken measurement report output did not fail");
    CHECK(!test_path_exists(files.third) &&
              test_contains(files.error, "cannot write standard output"),
          "broken measurement output published an artifact");
#endif
    CHECK(test_remove_file(files.audio) == 0 &&
              test_remove_file(files.items) == 0,
          "cannot remove prior-stage sources");
    {
        int compare_status = test_run(
            &files, compare, sizeof(compare) / sizeof(compare[0]));
        CHECK(compare_status == 0,
              "compare-measures reopened audio or item inputs (status %d)",
              compare_status);
        if (compare_status != 0) {
            size_t diagnostic_size;
            unsigned char *diagnostic = test_read_file(
                files.error, &diagnostic_size);
            if (diagnostic != NULL) {
                (void)fprintf(stderr, "%.*s", (int)diagnostic_size,
                              (const char *)diagnostic);
            }
            free(diagnostic);
        }
    }
    CHECK(test_contains(files.comparison, "HWA_COMPARE,1") &&
              test_contains(files.output, "Profile comparison"),
          "named comparison output is incomplete");
    CHECK(test_run(&files, compare_alias,
                   sizeof(compare_alias) / sizeof(compare_alias[0])) != 0,
          "comparison replaced an input profile");
    CHECK(test_run(&files, compare_cap,
                   sizeof(compare_cap) / sizeof(compare_cap[0])) != 0 &&
              !test_path_exists(files.third),
          "comparison work cap did not stop publication");
    CHECK(test_run(&files, compare_stdout,
                   sizeof(compare_stdout) / sizeof(compare_stdout[0])) == 0 &&
              test_contains(files.output, "HWA_COMPARE,1"),
          "canonical comparison standard output failed");
    CHECK(test_run(&files, compare_json,
                   sizeof(compare_json) / sizeof(compare_json[0])) == 0 &&
              test_contains(files.output, "\"schema_version\":6") &&
              test_contains(files.output, "\"distributions\":["),
          "comparison JSON report failed");
    free(items_before);
    free(audio_before);
    test_files_close(&files);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: stage4_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_measure_and_compare_cli();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 4 CLI test(s) failed\n", failures);
        return 1;
    }
    return 0;
}
