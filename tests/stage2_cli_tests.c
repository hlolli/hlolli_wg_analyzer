#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FIXTURE_RATE 8000U
#define FIXTURE_FRAMES 16000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char reference[PATH_MAX];
    char target[PATH_MAX];
    char score[PATH_MAX];
    char standard_output[PATH_MAX];
    char standard_error[PATH_MAX];
} TestWorkspace;

typedef int (*TestFunction)(void);

typedef struct TestCase {
    const char *name;
    TestFunction function;
} TestCase;

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

static long process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int join_path(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static int make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int workspace_open(TestWorkspace *workspace)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') {
        root = ".";
    }
#else
    const char *root = "/tmp";
#endif

    memset(workspace, 0, sizeof(*workspace));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(workspace->directory,
                              sizeof(workspace->directory),
                              "%s/hwa-stage2-cli-%ld-%u",
                              root, process_id(), attempt);
        if (length < 0 || (size_t)length >= sizeof(workspace->directory)) {
            return 0;
        }
        if (make_directory(workspace->directory) == 0) {
            break;
        }
        if (errno != EEXIST) {
            return 0;
        }
    }
    if (attempt == 100U ||
        !join_path(workspace->reference, workspace->directory, "reference.wav") ||
        !join_path(workspace->target, workspace->directory, "target.wav") ||
        !join_path(workspace->score, workspace->directory, "score.csv") ||
        !join_path(workspace->standard_output,
                   workspace->directory, "stdout.txt") ||
        !join_path(workspace->standard_error,
                   workspace->directory, "stderr.txt")) {
        (void)remove_directory(workspace->directory);
        return 0;
    }
    return 1;
}

static void workspace_close(TestWorkspace *workspace)
{
    static const char *const names[] = {
        "reference.wav", "target.wav", "score.csv", "stdout.txt",
        "stderr.txt", "one.hwa-align", "two.hwa-align",
        "three.hwa-align", "prior.hwa-align", "existing.hwa-align",
        "hardlink.hwa-align", "symlink.hwa-align", "output.fifo",
        "failed.hwa-align", "block-1.hwa-align", "block-257.hwa-align",
        "block-4096.hwa-align"
    };
    char path[PATH_MAX];
    size_t index;

    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (join_path(path, workspace->directory, names[index])) {
            (void)remove_file(path);
        }
    }
    (void)remove_directory(workspace->directory);
}

static int write_bytes(FILE *stream, const void *data, size_t size)
{
    return fwrite(data, 1U, size, stream) == size;
}

static int write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int16_t fixture_sample(uint32_t frame, int changed)
{
    static const double frequencies[4] = {
        261.6255653005986, 329.6275569128699,
        391.9954359817493, 293.6647679174076
    };
    uint32_t within = frame % 4000U;
    size_t note = (size_t)(frame / 4000U);
    double edge = (double)(within < 80U ? within :
                           (within > 3920U ? 4000U - within : 80U)) / 80.0;
    double frequency = frequencies[note];
    double phase;
    double sample;

    if (changed != 0 && note == 2U) {
        frequency *= 1.01;
    }
    phase = 2.0 * TEST_PI * frequency * (double)frame / (double)FIXTURE_RATE;
    sample = 0.45 * edge * sin(phase);
    if (sample > 1.0) sample = 1.0;
    if (sample < -1.0) sample = -1.0;
    return (int16_t)lrint(sample * 32767.0);
}

static int write_wav(const char *path, int changed)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) {
        return 0;
    }
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + FIXTURE_FRAMES * 2U) ||
        !write_bytes(stream, "WAVE", 4U) ||
        !write_bytes(stream, "fmt ", 4U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, 1U) || !write_u32(stream, FIXTURE_RATE) ||
        !write_u32(stream, FIXTURE_RATE * 2U) ||
        !write_u16(stream, 2U) || !write_u16(stream, 16U) ||
        !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, FIXTURE_FRAMES * 2U)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < FIXTURE_FRAMES; ++frame) {
        if (!write_u16(stream, (uint16_t)fixture_sample(frame, changed))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    int ok = stream != NULL && fwrite(text, 1U, size, stream) == size;

    if (stream != NULL && fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static int write_score(const char *path)
{
    static const char score[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n"
        "tempo,tempo,0,0,,,,,,,m1,120\n"
        "n1,note,0,1,60,88,solo,none,mf,plain,m1b1,\n"
        "n2,note,1,1,64,86,solo,none,p,dolce,m1b2,\n"
        "n3,note,2,1,67,92,solo,none,f,accent,m1b3,\n"
        "n4,note,3,1,62,84,solo,none,mf,tenuto,m1b4,";
    return write_text(path, score);
}

static int setup_fixture(TestWorkspace *workspace)
{
    CHECK(workspace_open(workspace), "could not make Stage 2 CLI workspace");
    if (failures != 0) {
        return 0;
    }
    CHECK(write_wav(workspace->reference, 0) &&
              write_wav(workspace->target, 0) &&
              write_score(workspace->score),
          "could not write Stage 2 fixtures");
    return failures == 0;
}

static int append_text(char *command,
                       size_t command_size,
                       size_t *length,
                       const char *text)
{
    size_t added = strlen(text);
    if (*length >= command_size || added >= command_size - *length) {
        return 0;
    }
    memcpy(command + *length, text, added + 1U);
    *length += added;
    return 1;
}

static int append_shell_argument(char *command,
                                 size_t command_size,
                                 size_t *length,
                                 const char *argument)
{
    const unsigned char *cursor = (const unsigned char *)argument;
#if defined(_WIN32)
    if (!append_text(command, command_size, length, "\"")) return 0;
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};
        if (*cursor == '"') {
            if (!append_text(command, command_size, length, "\\\"")) return 0;
        } else if (!append_text(command, command_size, length, character)) {
            return 0;
        }
        cursor++;
    }
    return append_text(command, command_size, length, "\"");
#else
    if (!append_text(command, command_size, length, "'")) return 0;
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};
        if (*cursor == '\'') {
            if (!append_text(command, command_size, length, "'\\''")) return 0;
        } else if (!append_text(command, command_size, length, character)) {
            return 0;
        }
        cursor++;
    }
    return append_text(command, command_size, length, "'");
#endif
}

static int run_analyzer(const TestWorkspace *workspace,
                        const char *const *arguments,
                        size_t argument_count)
{
    char command[PATH_MAX * 10U];
    size_t length = 0U;
    size_t index;
    int status;

    command[0] = '\0';
    if (!append_shell_argument(command, sizeof(command), &length,
                               analyzer_path)) {
        return -1;
    }
    for (index = 0U; index < argument_count; ++index) {
        if (!append_text(command, sizeof(command), &length, " ") ||
            !append_shell_argument(command, sizeof(command), &length,
                                   arguments[index])) {
            return -1;
        }
    }
    if (!append_text(command, sizeof(command), &length, " >") ||
        !append_shell_argument(command, sizeof(command), &length,
                               workspace->standard_output) ||
        !append_text(command, sizeof(command), &length, " 2>") ||
        !append_shell_argument(command, sizeof(command), &length,
                               workspace->standard_error)) {
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

static int run_audio_align(const TestWorkspace *workspace,
                           const char *output,
                           int json,
                           int replace,
                           const char *amend,
                           const char *block_frames,
                           const char *max_cells,
                           const char *max_work)
{
    const char *arguments[32];
    size_t count = 0U;

    if (json) arguments[count++] = "--json";
    if (replace) arguments[count++] = "--replace";
    arguments[count++] = "--frame-size"; arguments[count++] = "512";
    arguments[count++] = "--hop-size"; arguments[count++] = "128";
    arguments[count++] = "--alignment-step"; arguments[count++] = "0.05";
    arguments[count++] = "--coarse-step"; arguments[count++] = "0.20";
    arguments[count++] = "--dtw-band"; arguments[count++] = "2";
    arguments[count++] = "--fine-radius"; arguments[count++] = "0.40";
    arguments[count++] = "--refine-radius"; arguments[count++] = "0.10";
    if (amend != NULL) {
        arguments[count++] = "--amend"; arguments[count++] = amend;
    }
    if (block_frames != NULL) {
        arguments[count++] = "--block-frames";
        arguments[count++] = block_frames;
    }
    if (max_cells != NULL) {
        arguments[count++] = "--max-dtw-cells";
        arguments[count++] = max_cells;
    }
    if (max_work != NULL) {
        arguments[count++] = "--max-alignment-work-bytes";
        arguments[count++] = max_work;
    }
    arguments[count++] = "align";
    arguments[count++] = workspace->reference;
    arguments[count++] = workspace->target;
    arguments[count++] = "--output";
    arguments[count++] = output;
    return run_analyzer(workspace, arguments, count);
}

static int run_score_align(const TestWorkspace *workspace,
                           const char *output,
                           int json,
                           int replace,
                           const char *max_events)
{
    const char *arguments[28];
    size_t count = 0U;

    if (json) arguments[count++] = "--json";
    if (replace) arguments[count++] = "--replace";
    arguments[count++] = "--frame-size"; arguments[count++] = "512";
    arguments[count++] = "--hop-size"; arguments[count++] = "128";
    arguments[count++] = "--alignment-step"; arguments[count++] = "0.05";
    arguments[count++] = "--coarse-step"; arguments[count++] = "0.20";
    arguments[count++] = "--dtw-band"; arguments[count++] = "2";
    arguments[count++] = "--fine-radius"; arguments[count++] = "0.40";
    if (max_events != NULL) {
        arguments[count++] = "--max-score-events";
        arguments[count++] = max_events;
    }
    arguments[count++] = "align";
    arguments[count++] = "--score";
    arguments[count++] = workspace->score;
    arguments[count++] = workspace->target;
    arguments[count++] = "--output";
    arguments[count++] = output;
    return run_analyzer(workspace, arguments, count);
}

static char *read_file(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;

    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0 ||
        (uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(text, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    *size = (size_t)length;
    return text;
}

static uint64_t hash_file(const char *path, int *ok)
{
    unsigned char buffer[4096];
    uint64_t hash = UINT64_C(1469598103934665603);
    FILE *stream = fopen(path, "rb");
    size_t count;

    if (stream == NULL) {
        *ok = 0;
        return 0U;
    }
    while ((count = fread(buffer, 1U, sizeof(buffer), stream)) != 0U) {
        size_t index;
        for (index = 0U; index < count; ++index) {
            hash ^= buffer[index];
            hash *= UINT64_C(1099511628211);
        }
    }
    *ok = ferror(stream) == 0 && fclose(stream) == 0;
    return hash;
}

static void expect_success(int status, const TestWorkspace *workspace)
{
    size_t size;
    char *error = read_file(workspace->standard_error, &size);
    (void)size;
    CHECK(status == 0, "expected exit 0, got %d; stderr: %s",
          status, error != NULL ? error : "<unreadable>");
    free(error);
}

static void expect_failure(int status,
                           const TestWorkspace *workspace,
                           const char *word)
{
    size_t size;
    char *error = read_file(workspace->standard_error, &size);
    (void)size;
    CHECK(status == 1 || status == 2,
          "expected data/usage failure, got %d", status);
    CHECK(error != NULL && strstr(error, word) != NULL,
          "failure should mention '%s'; stderr: %s", word,
          error != NULL ? error : "<unreadable>");
    free(error);
}

static int canonical_basic(const char *text, const char *mode)
{
    return text != NULL &&
           strncmp(text, "HWA_ALIGNMENT,1\r\n", 17U) == 0 &&
           strstr(text, "META,analysis_method_version,stage1-1,\r\n") != NULL &&
           strstr(text, "META,alignment_method_version,stage2-1,\r\n") != NULL &&
           strstr(text, "META,build_compiler_family,") != NULL &&
           strstr(text, "META,build_pointer_bits,") != NULL &&
           strstr(text, mode) != NULL &&
           strstr(text, "ANCHOR,") != NULL &&
           strstr(text, "MATCH,") != NULL;
}

static size_t record_count(const char *text, const char *prefix)
{
    size_t count = 0U;
    size_t prefix_size = strlen(prefix);
    const char *line = text;

    while (line != NULL && *line != '\0') {
        if (strncmp(line, prefix, prefix_size) == 0) count++;
        line = strstr(line, "\r\n");
        if (line != NULL) line += 2U;
    }
    return count;
}

static int summary_matches_saved(const char *summary, const char *saved)
{
    const char *dtw_line = strstr(saved, "META,dtw_cells,");
    uint64_t dtw_cells;
    char expected[128];
    size_t matches = record_count(saved, "MATCH,");
    size_t unmatched = record_count(saved, "UNMATCHED,");
    size_t warnings = record_count(saved, "WARNING,");

    if (summary == NULL || dtw_line == NULL ||
        sscanf(dtw_line, "META,dtw_cells,%" SCNu64 ",cells", &dtw_cells) != 1) {
        return 0;
    }
    (void)snprintf(expected, sizeof(expected), "Matches: %zu\n", matches);
    if (strstr(summary, expected) == NULL) return 0;
    (void)snprintf(expected, sizeof(expected),
                   "Unmatched spans: %zu\n", unmatched);
    if (strstr(summary, expected) == NULL) return 0;
    (void)snprintf(expected, sizeof(expected), "Warnings: %zu\n", warnings);
    if (strstr(summary, expected) == NULL) return 0;
    (void)snprintf(expected, sizeof(expected),
                   "DTW cells: %" PRIu64 "\n", dtw_cells);
    return strstr(summary, expected) != NULL;
}

static void json_skip_space(JsonCursor *cursor)
{
    while (*cursor->current == (unsigned char)' ' ||
           *cursor->current == (unsigned char)'\t' ||
           *cursor->current == (unsigned char)'\r' ||
           *cursor->current == (unsigned char)'\n') {
        cursor->current++;
    }
}

static int json_string(JsonCursor *cursor)
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

static int json_value(JsonCursor *cursor);

static int json_array(JsonCursor *cursor)
{
    cursor->current++;
    json_skip_space(cursor);
    if (*cursor->current == (unsigned char)']') {
        cursor->current++;
        return 1;
    }
    for (;;) {
        if (!json_value(cursor)) return 0;
        json_skip_space(cursor);
        if (*cursor->current == (unsigned char)']') {
            cursor->current++;
            return 1;
        }
        if (*cursor->current++ != (unsigned char)',') return 0;
        json_skip_space(cursor);
    }
}

static int json_object(JsonCursor *cursor)
{
    cursor->current++;
    json_skip_space(cursor);
    if (*cursor->current == (unsigned char)'}') {
        cursor->current++;
        return 1;
    }
    for (;;) {
        if (!json_string(cursor)) return 0;
        json_skip_space(cursor);
        if (*cursor->current++ != (unsigned char)':') return 0;
        json_skip_space(cursor);
        if (!json_value(cursor)) return 0;
        json_skip_space(cursor);
        if (*cursor->current == (unsigned char)'}') {
            cursor->current++;
            return 1;
        }
        if (*cursor->current++ != (unsigned char)',') return 0;
        json_skip_space(cursor);
    }
}

static int json_number(JsonCursor *cursor)
{
    const unsigned char *start = cursor->current;
    if (*cursor->current == (unsigned char)'-') cursor->current++;
    if (*cursor->current == (unsigned char)'0') {
        cursor->current++;
    } else if (*cursor->current >= (unsigned char)'1' &&
               *cursor->current <= (unsigned char)'9') {
        do { cursor->current++; }
        while (isdigit((int)*cursor->current));
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
            *cursor->current == (unsigned char)'-') cursor->current++;
        if (!isdigit((int)*cursor->current)) return 0;
        while (isdigit((int)*cursor->current)) cursor->current++;
    }
    return cursor->current != start;
}

static int json_value(JsonCursor *cursor)
{
    int result;
    json_skip_space(cursor);
    if (cursor->depth++ > 64U) return 0;
    if (*cursor->current == (unsigned char)'{') result = json_object(cursor);
    else if (*cursor->current == (unsigned char)'[') result = json_array(cursor);
    else if (*cursor->current == (unsigned char)'"') result = json_string(cursor);
    else if (strncmp((const char *)cursor->current, "true", 4U) == 0) {
        cursor->current += 4U; result = 1;
    } else if (strncmp((const char *)cursor->current, "false", 5U) == 0) {
        cursor->current += 5U; result = 1;
    } else if (strncmp((const char *)cursor->current, "null", 4U) == 0) {
        cursor->current += 4U; result = 1;
    } else {
        result = json_number(cursor);
    }
    cursor->depth--;
    return result;
}

static int json_valid(const char *text)
{
    JsonCursor cursor;
    if (text == NULL) return 0;
    cursor.current = (const unsigned char *)text;
    cursor.depth = 0U;
    if (!json_value(&cursor)) return 0;
    json_skip_space(&cursor);
    return *cursor.current == 0U;
}

static int case_audio(void)
{
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    char *output;
    char *summary;
    size_t size;
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(output_path, workspace.directory, "one.hwa-align"),
          "audio output path is too long");
    status = run_audio_align(&workspace, output_path, 0, 0,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
    output = read_file(output_path, &size);
    (void)size;
    CHECK(canonical_basic(output, "META,mode,audio-audio,"),
          "audio/audio output is not a canonical Stage 2 file");
    summary = read_file(workspace.standard_output, &size);
    CHECK(output != NULL && summary_matches_saved(summary, output),
          "text summary counts do not match the saved alignment");
    free(summary);
    free(output);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_score(void)
{
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    char *output;
    size_t size;
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(output_path, workspace.directory, "one.hwa-align"),
          "score output path is too long");
    status = run_score_align(&workspace, output_path, 0, 0, NULL);
    expect_success(status, &workspace);
    output = read_file(output_path, &size);
    (void)size;
    CHECK(canonical_basic(output, "META,mode,score-audio,") &&
              strstr(output, "INPUT,score,") != NULL &&
              strstr(output, ",n1,note,solo,60,88,none,mf,plain,m1b1,120,") != NULL,
          "score/audio output lost score identity or note fields");
    free(output);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_json(void)
{
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    char *json;
    size_t size;
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(output_path, workspace.directory, "one.hwa-align"),
          "JSON output path is too long");
    status = run_audio_align(&workspace, output_path, 1, 0,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
    json = read_file(workspace.standard_output, &size);
    (void)size;
    CHECK(json_valid(json), "alignment summary is not valid JSON");
    CHECK(json != NULL &&
              strstr(json, "\"schema_version\":3") != NULL &&
              strstr(json, "\"command\":\"align\"") != NULL &&
              strstr(json, "\"analysis_method_version\":\"stage1-1\"") != NULL &&
              strstr(json, "\"method_version\":\"stage2-1\"") != NULL &&
              strstr(json, "\"build\":{") != NULL &&
              strstr(json, "\"compiler_family\":\"") != NULL &&
              strstr(json, "\"compiler_version\":\"") != NULL &&
              strstr(json, "\"c_standard\":\"") != NULL &&
              strstr(json, "\"target_os\":\"") != NULL &&
              strstr(json, "\"pointer_bits\":") != NULL &&
              strstr(json, "\"endianness\":\"") != NULL &&
              strstr(json, "\"mode\":\"") != NULL &&
              strstr(json, "\"mode\":\"audio_audio\"") != NULL,
          "alignment JSON lacks schema, method, or build facts");
    free(json);
    workspace_close(&workspace);
    return failures == 0;
}

static int lock_first_interior(const char *source_path,
                               const char *prior_path,
                               double *reference,
                               double *target)
{
    size_t size;
    char *text = read_file(source_path, &size);
    char *line;
    char *result;
    int found = 0;

    if (text == NULL) return 0;
    line = text;
    while (line < text + size) {
        char *end = strstr(line, "\r\n");
        if (end == NULL) end = text + size;
        if (strncmp(line, "ANCHOR,", 7U) == 0) {
            uint64_t id;
            double ref;
            double tgt;
            char saved = *end;
            *end = '\0';
            if (sscanf(line, "ANCHOR,%" SCNu64 ",%lf,%lf,",
                       &id, &ref, &tgt) == 3 && ref > 0.1 && ref < 1.9 &&
                strstr(line, ",auto,0,") != NULL) {
                char *mark = strstr(line, ",auto,0,");
                size_t prefix = (size_t)(mark - text);
                static const char replacement[] = ",manual,1,";
                size_t old_size = strlen(",auto,0,");
                size_t new_size = sizeof(replacement) - 1U;

                *end = saved;
                result = (char *)malloc(size - old_size + new_size + 1U);
                if (result == NULL) {
                    free(text);
                    return 0;
                }
                memcpy(result, text, prefix);
                memcpy(result + prefix, replacement, new_size);
                memcpy(result + prefix + new_size, mark + old_size,
                       size - prefix - old_size);
                result[size - old_size + new_size] = '\0';
                found = write_text(prior_path, result);
                free(result);
                *reference = ref;
                *target = tgt;
                break;
            }
            *end = saved;
        }
        line = end + (end < text + size ? 2 : 0);
    }
    free(text);
    return found;
}

static int has_manual_anchor(const char *path,
                             double wanted_reference,
                             double wanted_target)
{
    size_t size;
    char *text = read_file(path, &size);
    char *line;
    int found = 0;

    if (text == NULL) return 0;
    line = text;
    while (line < text + size) {
        char *end = strstr(line, "\r\n");
        if (end == NULL) end = text + size;
        if (strncmp(line, "ANCHOR,", 7U) == 0) {
            uint64_t id;
            double ref;
            double tgt;
            char saved = *end;
            *end = '\0';
            if (sscanf(line, "ANCHOR,%" SCNu64 ",%lf,%lf,",
                       &id, &ref, &tgt) == 3 &&
                fabs(ref - wanted_reference) <= 1e-12 &&
                fabs(tgt - wanted_target) <= 1e-12 &&
                strstr(line, ",manual,1,") != NULL) {
                found = 1;
            }
            *end = saved;
        }
        line = end + (end < text + size ? 2 : 0);
    }
    free(text);
    return found;
}

static int case_amend(void)
{
    TestWorkspace workspace;
    char first[PATH_MAX];
    char prior[PATH_MAX];
    char second[PATH_MAX];
    double reference = 0.0;
    double target = 0.0;
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(first, workspace.directory, "one.hwa-align") &&
              join_path(prior, workspace.directory, "prior.hwa-align") &&
              join_path(second, workspace.directory, "two.hwa-align"),
          "amend paths are too long");
    status = run_audio_align(&workspace, first, 0, 0,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
    CHECK(lock_first_interior(first, prior, &reference, &target),
          "could not make an editable locked-anchor fixture");
    status = run_audio_align(&workspace, second, 0, 0,
                             prior, NULL, NULL, NULL);
    expect_success(status, &workspace);
    CHECK(has_manual_anchor(second, reference, target),
          "amend did not keep the locked anchor exactly");
    workspace_close(&workspace);
    return failures == 0;
}

static char *without_decode_block(const char *text)
{
    static const char prefix[] = "META,decode_block_frames,";
    const char *line = strstr(text, prefix);
    const char *end;
    size_t before;
    size_t after;
    char *result;

    if (line == NULL || (end = strstr(line, "\r\n")) == NULL) return NULL;
    end += 2U;
    before = (size_t)(line - text);
    after = strlen(end);
    result = (char *)malloc(before + after + 1U);
    if (result == NULL) return NULL;
    memcpy(result, text, before);
    memcpy(result + before, end, after + 1U);
    return result;
}

static int case_block_invariance(void)
{
    TestWorkspace workspace;
    static const char *const blocks[3] = {"1", "257", "4096"};
    static const char *const names[3] = {
        "block-1.hwa-align", "block-257.hwa-align", "block-4096.hwa-align"
    };
    char path[PATH_MAX];
    char *baseline = NULL;
    size_t index;

    if (!setup_fixture(&workspace)) return 0;
    for (index = 0U; index < 3U; ++index) {
        char *text;
        char *normalized;
        size_t size;
        int status;

        CHECK(join_path(path, workspace.directory, names[index]),
              "block output path is too long");
        status = run_audio_align(&workspace, path, 0, 0,
                                 NULL, blocks[index], NULL, NULL);
        expect_success(status, &workspace);
        text = read_file(path, &size);
        (void)size;
        normalized = text != NULL ? without_decode_block(text) : NULL;
        CHECK(normalized != NULL, "could not normalize block metadata");
        if (index == 0U) {
            baseline = normalized;
        } else {
            CHECK(baseline != NULL && normalized != NULL &&
                      strcmp(baseline, normalized) == 0,
                  "alignment changed with decode block size %s", blocks[index]);
            free(normalized);
        }
        free(text);
    }
    free(baseline);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_limits(void)
{
    TestWorkspace workspace;
    char output[PATH_MAX];
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(output, workspace.directory, "one.hwa-align"),
          "limit output path is too long");
    status = run_audio_align(&workspace, output, 0, 0,
                             NULL, NULL, "1", NULL);
    expect_failure(status, &workspace, "DTW cell limit exceeded");
    CHECK(fopen(output, "rb") == NULL,
          "failed DTW-cell run left an output file");
    status = run_audio_align(&workspace, output, 0, 0,
                             NULL, NULL, NULL, "1024");
    expect_failure(status, &workspace, "work byte limit exceeded");
    status = run_score_align(&workspace, output, 0, 0, "4");
    expect_failure(status, &workspace, "event limit");
    status = run_score_align(&workspace, output, 0, 0, "5");
    expect_success(status, &workspace);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_output_safety(void)
{
    static const char sentinel[] = "keep this output\n";
    TestWorkspace workspace;
    char existing[PATH_MAX];
    char prior[PATH_MAX];
    char second[PATH_MAX];
    uint64_t before;
    uint64_t after;
    uint64_t reference_hash_initial;
    uint64_t target_hash_initial;
    uint64_t score_hash_initial;
    int ok;
    int status;
#if !defined(_WIN32)
    char hardlink_path[PATH_MAX];
    char symlink_path[PATH_MAX];
    char fifo_path[PATH_MAX];
    struct stat file_status;
    struct stat reference_status;
    struct stat reference_after_status;
    struct stat target_status;
    struct stat target_after_status;
    struct stat score_status;
    struct stat score_after_status;
#endif

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(existing, workspace.directory, "existing.hwa-align") &&
              join_path(prior, workspace.directory, "prior.hwa-align") &&
              join_path(second, workspace.directory, "two.hwa-align"),
          "safety paths are too long");
    reference_hash_initial = hash_file(workspace.reference, &ok);
    CHECK(ok, "could not hash reference before safety checks");
    target_hash_initial = hash_file(workspace.target, &ok);
    CHECK(ok, "could not hash target before safety checks");
    score_hash_initial = hash_file(workspace.score, &ok);
    CHECK(ok, "could not hash score before safety checks");
#if !defined(_WIN32)
    CHECK(stat(workspace.reference, &reference_status) == 0 &&
              stat(workspace.target, &target_status) == 0 &&
              stat(workspace.score, &score_status) == 0,
          "could not inspect input metadata");
#endif
    status = run_audio_align(&workspace, workspace.reference, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "input");
    after = hash_file(workspace.reference, &ok);
    CHECK(ok && after == reference_hash_initial,
          "same-path output changed the reference");
    before = hash_file(workspace.target, &ok);
    CHECK(ok, "could not hash target before safety checks");
    status = run_audio_align(&workspace, workspace.target, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "input");
    after = hash_file(workspace.target, &ok);
    CHECK(ok && after == before, "same-path output changed the target");

    CHECK(write_text(existing, sentinel), "could not write output sentinel");
#if !defined(_WIN32)
    CHECK(chmod(existing, 0600) == 0, "could not set output mode");
#endif
    status = run_audio_align(&workspace, existing, 0, 0,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "cannot create output");
    {
        size_t size;
        char *text = read_file(existing, &size);
        (void)size;
        CHECK(text != NULL && strcmp(text, sentinel) == 0,
              "refused overwrite changed the sentinel");
        free(text);
    }
    status = run_audio_align(&workspace, existing, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
#if !defined(_WIN32)
    CHECK(stat(existing, &file_status) == 0 &&
              (file_status.st_mode & 0777) == 0600,
          "atomic replacement did not keep mode 0600");
    CHECK(join_path(hardlink_path, workspace.directory, "hardlink.hwa-align") &&
              join_path(symlink_path, workspace.directory, "symlink.hwa-align") &&
              join_path(fifo_path, workspace.directory, "output.fifo"),
          "special output paths are too long");
    CHECK(link(workspace.target, hardlink_path) == 0,
          "could not make protected hard link");
    status = run_audio_align(&workspace, hardlink_path, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "input");
    CHECK(symlink(existing, symlink_path) == 0,
          "could not make output symlink");
    before = hash_file(existing, &ok);
    status = run_audio_align(&workspace, symlink_path, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "regular file");
    after = hash_file(existing, &ok);
    CHECK(ok && before == after,
          "symlink output refusal changed its target");
    CHECK(mkfifo(fifo_path, 0600) == 0, "could not make output FIFO");
    status = run_audio_align(&workspace, fifo_path, 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "regular file");
#endif

    status = run_score_align(&workspace, workspace.score, 0, 1, NULL);
    expect_failure(status, &workspace, "input");
    after = hash_file(workspace.score, &ok);
    CHECK(ok && after == score_hash_initial,
          "protected score bytes changed during output refusal");

    status = run_audio_align(&workspace, prior, 0, 0,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
    before = hash_file(prior, &ok);
    status = run_audio_align(&workspace, prior, 0, 1,
                             prior, NULL, NULL, NULL);
    expect_failure(status, &workspace, "input");
    after = hash_file(prior, &ok);
    CHECK(ok && before == after, "amend/output alias changed the prior file");

#if !defined(_WIN32)
    CHECK(stat(workspace.reference, &reference_after_status) == 0 &&
              stat(workspace.target, &target_after_status) == 0 &&
              stat(workspace.score, &score_after_status) == 0 &&
              reference_after_status.st_dev == reference_status.st_dev &&
              reference_after_status.st_ino == reference_status.st_ino &&
              reference_after_status.st_size == reference_status.st_size &&
              reference_after_status.st_mtime == reference_status.st_mtime &&
              (reference_after_status.st_mode & 07777) ==
                  (reference_status.st_mode & 07777) &&
              target_after_status.st_dev == target_status.st_dev &&
              target_after_status.st_ino == target_status.st_ino &&
              target_after_status.st_size == target_status.st_size &&
              target_after_status.st_mtime == target_status.st_mtime &&
              (target_after_status.st_mode & 07777) ==
                  (target_status.st_mode & 07777) &&
              score_after_status.st_dev == score_status.st_dev &&
              score_after_status.st_ino == score_status.st_ino &&
              score_after_status.st_size == score_status.st_size &&
              score_after_status.st_mtime == score_status.st_mtime &&
              (score_after_status.st_mode & 07777) ==
                  (score_status.st_mode & 07777),
          "alignment changed input file metadata");
#endif
    CHECK(hash_file(workspace.reference, &ok) == reference_hash_initial && ok &&
              hash_file(workspace.target, &ok) == target_hash_initial && ok &&
              hash_file(workspace.score, &ok) == score_hash_initial && ok,
          "alignment changed input bytes");

    status = run_audio_align(&workspace, "-", 0, 0,
                             NULL, NULL, NULL, NULL);
    expect_success(status, &workspace);
    {
        size_t size;
        char *text = read_file(workspace.standard_output, &size);
        (void)size;
        CHECK(canonical_basic(text, "META,mode,audio-audio,"),
              "--output - did not write the canonical alignment");
        free(text);
    }
    status = run_audio_align(&workspace, "-", 1, 0,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "invalid command options");
    status = run_audio_align(&workspace, "-", 0, 1,
                             NULL, NULL, NULL, NULL);
    expect_failure(status, &workspace, "invalid command options");
    {
        const char *arguments[] = {
            "align", workspace.reference, "-", "--output", second
        };
        status = run_analyzer(&workspace, arguments,
                              sizeof(arguments) / sizeof(arguments[0]));
        expect_failure(status, &workspace, "named regular inputs");
    }
    workspace_close(&workspace);
    return failures == 0;
}

#if !defined(_WIN32)
static int temporary_output_count(const char *directory)
{
    DIR *entries = opendir(directory);
    struct dirent *entry;
    int count = 0;

    if (entries == NULL) return -1;
    while ((entry = readdir(entries)) != NULL) {
        if (strstr(entry->d_name, ".hwa-tmp-") != NULL) count++;
    }
    if (closedir(entries) != 0) return -1;
    return count;
}

static int run_with_file_limit(const TestWorkspace *workspace,
                               const char *output,
                               rlim_t limit_value)
{
    const char *arguments[24];
    char *exec_arguments[26];
    size_t count = 0U;
    size_t index;
    pid_t process;
    int status;
    struct rlimit limit;

    arguments[count++] = "--frame-size"; arguments[count++] = "512";
    arguments[count++] = "--hop-size"; arguments[count++] = "128";
    arguments[count++] = "--alignment-step"; arguments[count++] = "0.05";
    arguments[count++] = "--coarse-step"; arguments[count++] = "0.20";
    arguments[count++] = "--dtw-band"; arguments[count++] = "2";
    arguments[count++] = "--fine-radius"; arguments[count++] = "0.40";
    arguments[count++] = "align";
    arguments[count++] = workspace->reference;
    arguments[count++] = workspace->target;
    arguments[count++] = "--output";
    arguments[count++] = output;
    process = fork();
    if (process < 0) return -1;
    if (process == 0) {
        int out = open(workspace->standard_output,
                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int err = open(workspace->standard_error,
                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 ||
            dup2(err, STDERR_FILENO) < 0) _exit(126);
        (void)close(out);
        (void)close(err);
        limit.rlim_cur = limit_value;
        limit.rlim_max = limit_value;
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
            setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(126);
        exec_arguments[0] = (char *)analyzer_path;
        for (index = 0U; index < count; ++index) {
            exec_arguments[index + 1U] = (char *)arguments[index];
        }
        exec_arguments[count + 1U] = NULL;
        (void)execv(analyzer_path, exec_arguments);
        _exit(127);
    }
    do {
        if (waitpid(process, &status, 0) == process) break;
    } while (errno == EINTR);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int case_output_failure(void)
{
    TestWorkspace workspace;
    char output[PATH_MAX];
    int status;

    if (!setup_fixture(&workspace)) return 0;
    CHECK(join_path(output, workspace.directory, "failed.hwa-align"),
          "failed-output path is too long");
    status = run_with_file_limit(&workspace, output, (rlim_t)256U);
    CHECK(status == 1, "short write should exit 1, got %d", status);
    CHECK(access(output, F_OK) != 0,
          "short write left a partial named output");
    CHECK(temporary_output_count(workspace.directory) == 0,
          "short write left a temporary output");
    workspace_close(&workspace);
    return failures == 0;
}
#endif

int main(int argc, char **argv)
{
    static const TestCase cases[] = {
        {"stage2-audio", case_audio},
        {"stage2-score", case_score},
        {"stage2-json", case_json},
        {"stage2-amend", case_amend},
        {"stage2-block-invariance", case_block_invariance},
        {"stage2-limits", case_limits},
        {"stage2-output-safety", case_output_safety},
#if !defined(_WIN32)
        {"stage2-output-failure", case_output_failure},
#endif
    };
    size_t index;

    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s ANALYZER CASE\n", argv[0]);
        return 2;
    }
    analyzer_path = argv[1];
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        if (strcmp(argv[2], cases[index].name) == 0) {
            int result = cases[index].function();
            if (result == 77) return 77;
            return failures == 0 ? 0 : 1;
        }
    }
    (void)fprintf(stderr, "unknown Stage 2 CLI case: %s\n", argv[2]);
    return 2;
}
