#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

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
#include <sys/stat.h>
#include "windows_test_process.h"
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
#define FIXTURE_FRAMES 32000U
#define FIXTURE_BYTES (44U + FIXTURE_FRAMES * 4U)

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char input[PATH_MAX];
    char standard_output[PATH_MAX];
    char standard_error[PATH_MAX];
} TestWorkspace;

typedef int (*TestFunction)(void);

typedef struct TestCase {
    const char *name;
    TestFunction function;
} TestCase;

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

static int join_path(char *path,
                     size_t path_size,
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, path_size, "%s/%s", directory, name);

    return length >= 0 && (size_t)length < path_size;
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
    const char *temporary_root = getenv("TEMP");
    if (temporary_root == NULL || temporary_root[0] == '\0') {
        temporary_root = ".";
    }
#else
    const char *temporary_root = "/tmp";
#endif

    memset(workspace, 0, sizeof(*workspace));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(
            workspace->directory, sizeof(workspace->directory),
            "%s/hwa-stage1-cli-%ld-%u", temporary_root,
            test_process_id(), attempt);

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
        !join_path(workspace->input, sizeof(workspace->input),
                   workspace->directory, "stage1.wav") ||
        !join_path(workspace->standard_output,
                   sizeof(workspace->standard_output),
                   workspace->directory, "stdout.txt") ||
        !join_path(workspace->standard_error,
                   sizeof(workspace->standard_error),
                   workspace->directory, "stderr.txt")) {
        (void)remove_directory(workspace->directory);
        return 0;
    }
    return 1;
}

static void workspace_close(TestWorkspace *workspace)
{
    (void)remove_file(workspace->input);
    (void)remove_file(workspace->standard_output);
    (void)remove_file(workspace->standard_error);
    (void)remove_directory(workspace->directory);
}

static int write_bytes(FILE *file, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, file) == size;
}

static int write_u16(FILE *file, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u32(FILE *file, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    return write_bytes(file, bytes, sizeof(bytes));
}

static int16_t fixture_sample(uint32_t frame, unsigned channel)
{
    static const int16_t sine[16] = {
        0, 6270, 11585, 15137, 16384, 15137, 11585, 6270,
        0, -6270, -11585, -15137, -16384, -15137, -11585, -6270
    };
    int16_t left;

    if (frame < 4000U || frame >= 28000U) {
        return 0;
    }
    left = sine[(frame - 4000U) % 16U];
    return channel == 0U ? left : (int16_t)(-(left / 2));
}

static int write_fixture(const char *path)
{
    uint32_t frame;
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return 0;
    }
    if (!write_bytes(file, "RIFF", 4U) ||
        !write_u32(file, 36U + FIXTURE_FRAMES * 4U) ||
        !write_bytes(file, "WAVE", 4U) ||
        !write_bytes(file, "fmt ", 4U) ||
        !write_u32(file, 16U) ||
        !write_u16(file, 1U) ||
        !write_u16(file, 2U) ||
        !write_u32(file, FIXTURE_RATE) ||
        !write_u32(file, FIXTURE_RATE * 4U) ||
        !write_u16(file, 4U) ||
        !write_u16(file, 16U) ||
        !write_bytes(file, "data", 4U) ||
        !write_u32(file, FIXTURE_FRAMES * 4U)) {
        (void)fclose(file);
        return 0;
    }
    for (frame = 0U; frame < FIXTURE_FRAMES; ++frame) {
        if (!write_u16(file, (uint16_t)fixture_sample(frame, 0U)) ||
            !write_u16(file, (uint16_t)fixture_sample(frame, 1U))) {
            (void)fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}

#if !defined(_WIN32)
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
    if (!append_text(command, command_size, length, "\"")) {
        return 0;
    }
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};

        if (*cursor == '"') {
            if (!append_text(command, command_size, length, "\\\"")) {
                return 0;
            }
        } else if (!append_text(command, command_size, length, character)) {
            return 0;
        }
        cursor++;
    }
    return append_text(command, command_size, length, "\"");
#else
    if (!append_text(command, command_size, length, "'")) {
        return 0;
    }
    while (*cursor != 0U) {
        char character[2] = {(char)*cursor, '\0'};

        if (*cursor == '\'') {
            if (!append_text(command, command_size, length, "'\\''")) {
                return 0;
            }
        } else if (!append_text(command, command_size, length, character)) {
            return 0;
        }
        cursor++;
    }
    return append_text(command, command_size, length, "'");
#endif
}
#endif

static int run_analyzer(const TestWorkspace *workspace,
                        const char *const *arguments,
                        size_t argument_count,
                        const char *standard_input)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(
        analyzer_path, arguments, argument_count, standard_input,
        workspace->standard_output, workspace->standard_error);
#else
    char command[PATH_MAX * 6U];
    size_t length = 0U;
    size_t index;
    int status;

    command[0] = '\0';
    if (!append_shell_argument(command, sizeof(command),
                               &length, analyzer_path)) {
        return -1;
    }
    for (index = 0U; index < argument_count; ++index) {
        if (!append_text(command, sizeof(command), &length, " ") ||
            !append_shell_argument(command, sizeof(command),
                                   &length, arguments[index])) {
            return -1;
        }
    }
    if (standard_input != NULL &&
        (!append_text(command, sizeof(command), &length, " <") ||
         !append_shell_argument(command, sizeof(command),
                                &length, standard_input))) {
        return -1;
    }
    if (!append_text(command, sizeof(command), &length, " >") ||
        !append_shell_argument(command, sizeof(command),
                               &length, workspace->standard_output) ||
        !append_text(command, sizeof(command), &length, " 2>") ||
        !append_shell_argument(command, sizeof(command),
                               &length, workspace->standard_error)) {
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

#if !defined(_WIN32)
static int run_analyzer_with_file_limit(const TestWorkspace *workspace,
                                        const char *const *arguments,
                                        size_t argument_count,
                                        rlim_t file_limit)
{
    char *exec_arguments[17];
    struct rlimit limit;
    pid_t process;
    pid_t waited;
    size_t index;
    int status;

    if (argument_count > 15U) {
        return -1;
    }
    process = fork();
    if (process < 0) {
        return -1;
    }
    if (process == 0) {
        int output_descriptor = open(workspace->standard_output,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int error_descriptor = open(workspace->standard_error,
                                    O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (output_descriptor < 0 || error_descriptor < 0 ||
            dup2(output_descriptor, STDOUT_FILENO) < 0 ||
            dup2(error_descriptor, STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(output_descriptor);
        (void)close(error_descriptor);
        limit.rlim_cur = file_limit;
        limit.rlim_max = file_limit;
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
            setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            _exit(126);
        }
        exec_arguments[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index) {
            exec_arguments[index + 1U] = (char *)arguments[index];
        }
        exec_arguments[argument_count + 1U] = NULL;
        (void)execv(analyzer_path, exec_arguments);
        _exit(127);
    }
    do {
        waited = waitpid(process, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != process || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static int temporary_output_count(const char *directory)
{
    DIR *entries = opendir(directory);
    struct dirent *entry;
    int count = 0;

    if (entries == NULL) {
        return -1;
    }
    while ((entry = readdir(entries)) != NULL) {
        if (strstr(entry->d_name, ".hwa-tmp-") != NULL) {
            count++;
        }
    }
    if (closedir(entries) != 0) {
        return -1;
    }
    return count;
}
#endif

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long length;
    char *text;

    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(text, 1U, (size_t)length, file) != (size_t)length) {
        free(text);
        (void)fclose(file);
        return NULL;
    }
    text[length] = '\0';
    if (fclose(file) != 0) {
        free(text);
        return NULL;
    }
    return text;
}

static const char *json_value(const char *json, const char *key)
{
    char token[128];
    int length = snprintf(token, sizeof(token), "\"%s\"", key);
    const char *value;

    if (length < 0 || (size_t)length >= sizeof(token)) {
        return NULL;
    }
    value = strstr(json, token);
    if (value == NULL) {
        return NULL;
    }
    value += (size_t)length;
    while (*value == ' ' || *value == '\t' ||
           *value == '\r' || *value == '\n') {
        value++;
    }
    if (*value++ != ':') {
        return NULL;
    }
    while (*value == ' ' || *value == '\t' ||
           *value == '\r' || *value == '\n') {
        value++;
    }
    return value;
}

static int json_number(const char *json, const char *key, double *number)
{
    const char *value = json_value(json, key);
    char *end;

    if (value == NULL) {
        return 0;
    }
    errno = 0;
    *number = strtod(value, &end);
    return end != value && errno != ERANGE && isfinite(*number);
}

static int json_string_is(const char *json,
                          const char *key,
                          const char *expected)
{
    const char *value = json_value(json, key);
    size_t length = strlen(expected);

    return value != NULL && value[0] == '"' &&
           strncmp(value + 1U, expected, length) == 0 &&
           value[length + 1U] == '"';
}

static int near(double left, double right, double tolerance)
{
    return fabs(left - right) <= tolerance;
}

static uint64_t hash_file(const char *path, int *ok)
{
    unsigned char buffer[4096];
    uint64_t hash = UINT64_C(1469598103934665603);
    FILE *file = fopen(path, "rb");
    size_t count;

    if (file == NULL) {
        *ok = 0;
        return 0U;
    }
    while ((count = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
        size_t index;

        for (index = 0U; index < count; ++index) {
            hash ^= buffer[index];
            hash *= UINT64_C(1099511628211);
        }
    }
    *ok = ferror(file) == 0 && fclose(file) == 0;
    return hash;
}

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t length = strlen(text);
    int result;

    if (file == NULL) {
        return 0;
    }
    result = fwrite(text, 1U, length, file) == length;
    if (fclose(file) != 0) {
        result = 0;
    }
    return result;
}

static int line_count(const char *text)
{
    int count = 0;

    while (*text != '\0') {
        if (*text++ == '\n') {
            count++;
        }
    }
    return count;
}

static int setup_fixture(TestWorkspace *workspace)
{
    CHECK(workspace_open(workspace), "could not create a test workspace");
    if (failures != 0) {
        return 0;
    }
    CHECK(write_fixture(workspace->input),
          "could not write the deterministic stereo fixture");
    return failures == 0;
}

static void expect_success(int status, const TestWorkspace *workspace)
{
    char *error = read_file(workspace->standard_error);

    CHECK(status == 0, "expected exit 0, got %d; stderr: %s",
          status, error != NULL ? error : "<unreadable>");
    free(error);
}

static void expect_data_failure(int status,
                                const TestWorkspace *workspace,
                                const char *word)
{
    char *error = read_file(workspace->standard_error);

    CHECK(status == 1, "expected data exit 1, got %d", status);
    CHECK(error != NULL && strstr(error, word) != NULL,
          "failure should mention '%s'; stderr: %s", word,
          error != NULL ? error : "<unreadable>");
    free(error);
}

static int case_schema(void)
{
    TestWorkspace workspace;
    const char *arguments[] = {"--json", "inspect", NULL};
    char *output;
    double value = 0.0;
    double peak = 0.0;
    double true_peak = 0.0;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    arguments[2] = workspace.input;
    status = run_analyzer(&workspace, arguments, 3U, NULL);
    expect_success(status, &workspace);
    output = read_file(workspace.standard_output);
    CHECK(output != NULL && json_number(output, "schema_version", &value) &&
              value == 2.0,
          "inspect must use JSON schema version 2");
    CHECK(output != NULL && json_string_is(output, "command", "inspect"),
          "schema command must be inspect");
    CHECK(output != NULL && json_string_is(output, "method_version", "stage1-1"),
          "schema must state the Stage 1 method version");
    CHECK(output != NULL && json_number(output, "analyzed_channels", &value) &&
              value == 2.0,
          "default analysis must keep both channels");
    CHECK(output != NULL && json_number(output, "peak", &peak) &&
              near(peak, 0.5, 1e-12),
          "left peak must be 0.5");
    CHECK(output != NULL && json_number(output, "true_peak", &true_peak) &&
              true_peak >= peak,
          "true peak must be present and no lower than sample peak");
    CHECK(output != NULL && strstr(output, "\"loudness\":{") != NULL &&
              strstr(output, "BS.1770-4 and EBU R128 style") != NULL &&
              json_number(output, "integrated_lufs", &value),
          "schema must contain a finite named loudness result");
    CHECK(output != NULL && strstr(output, "\"loudness_range_lu\":") != NULL &&
              json_number(output, "momentary_max_lufs", &value) &&
              json_number(output, "short_term_max_lufs", &value),
          "schema must contain global loudness fields");
    CHECK(output != NULL && strstr(output, "\"spectrum\":{") != NULL &&
              json_number(output, "centroid_hz", &value) && value > 0.0 &&
              json_number(output, "spread_hz", &value) &&
              json_number(output, "rolloff_85_hz", &value) &&
              json_number(output, "flatness", &value) &&
              json_number(output, "mean_flux", &value),
          "schema must contain finite global spectrum fields");
    CHECK(output != NULL && strstr(output, "\"band_power\":[") != NULL &&
              json_number(output, "transform_count", &value) && value > 0.0,
          "schema must contain spectrum bands and a transform count");
    CHECK(output != NULL && strstr(output, "\"activity\":{") != NULL &&
              json_number(output, "silence_fraction", &value) &&
              value > 0.0 && value < 1.0 &&
              json_number(output, "active_start_seconds", &value) &&
              json_number(output, "active_end_seconds", &value),
          "schema must contain finite activity results");
    CHECK(output != NULL && strstr(output, "\"stereo\":{") != NULL &&
              json_number(output, "correlation", &value),
          "schema must contain stereo results");
    free(output);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_channels(void)
{
    TestWorkspace workspace;
    const char *channel_arguments[] = {
        "--json", "--channel", "2", "inspect", NULL
    };
    const char *mix_arguments[] = {"--json", "--mixdown", "inspect", NULL};
    char *channel_output;
    char *mix_output;
    double channel_peak = 0.0;
    double mix_peak = 0.0;
    double value;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    channel_arguments[4] = workspace.input;
    status = run_analyzer(&workspace, channel_arguments, 5U, NULL);
    expect_success(status, &workspace);
    channel_output = read_file(workspace.standard_output);
    CHECK(channel_output != NULL &&
              json_string_is(channel_output, "channel_mode", "select") &&
              json_number(channel_output, "selected_channel", &value) &&
              value == 2.0 &&
              json_number(channel_output, "analyzed_channels", &value) &&
              value == 1.0,
          "--channel 2 must report one selected channel");
    CHECK(channel_output != NULL &&
              json_number(channel_output, "peak", &channel_peak) &&
              near(channel_peak, 0.25, 1e-12),
          "selected right-channel peak must be 0.25");

    mix_arguments[3] = workspace.input;
    status = run_analyzer(&workspace, mix_arguments, 4U, NULL);
    expect_success(status, &workspace);
    mix_output = read_file(workspace.standard_output);
    CHECK(mix_output != NULL &&
              json_string_is(mix_output, "channel_mode", "mix") &&
              json_number(mix_output, "analyzed_channels", &value) &&
              value == 1.0,
          "--mixdown must report one mixed channel");
    CHECK(mix_output != NULL && json_number(mix_output, "peak", &mix_peak) &&
              near(mix_peak, 0.125, 1e-12),
          "equal mix peak must be 0.125");
    CHECK(near(channel_peak, mix_peak * 2.0, 1e-12),
          "known channel difference must survive channel and mix rules");
    free(channel_output);
    free(mix_output);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_limits(void)
{
    TestWorkspace workspace;
    const char *frame_arguments[] = {
        "--max-frames", "31999", "inspect", NULL
    };
    const char *byte_arguments[] = {
        "--max-bytes", "128043", "inspect", NULL
    };
    const char *work_arguments[] = {
        "--max-transforms", "1", "inspect", NULL
    };
    const char *work_byte_fail_arguments[] = {
        "--max-work-bytes", "1024", "inspect", NULL
    };
    const char *work_byte_pass_arguments[] = {
        "--max-work-bytes", "268435456", "--json", "inspect", NULL
    };
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    frame_arguments[3] = workspace.input;
    status = run_analyzer(&workspace, frame_arguments, 4U, NULL);
    expect_data_failure(status, &workspace, "frame");
    byte_arguments[3] = workspace.input;
    status = run_analyzer(&workspace, byte_arguments, 4U, NULL);
    expect_data_failure(status, &workspace, "byte limit");
    work_arguments[3] = workspace.input;
    status = run_analyzer(&workspace, work_arguments, 4U, NULL);
    expect_data_failure(status, &workspace, "transform limit");
    work_byte_fail_arguments[3] = workspace.input;
    status = run_analyzer(&workspace, work_byte_fail_arguments, 4U, NULL);
    expect_data_failure(status, &workspace, "work limit");
    work_byte_pass_arguments[4] = workspace.input;
    status = run_analyzer(&workspace, work_byte_pass_arguments, 5U, NULL);
    expect_success(status, &workspace);
    workspace_close(&workspace);
    return failures == 0;
}

#if !defined(_WIN32)
static int case_stdin(void)
{
    TestWorkspace workspace;
    const char *arguments[] = {
        "--json", "--max-bytes", "128044", "inspect", "-"
    };
    char *output;
    double value;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    status = run_analyzer(&workspace, arguments, 5U, workspace.input);
    expect_success(status, &workspace);
    output = read_file(workspace.standard_output);
    CHECK(output != NULL && json_string_is(output, "path", "-") &&
              json_number(output, "frames", &value) &&
              value == (double)FIXTURE_FRAMES,
          "stdin inspect must parse the complete bounded WAVE stream");
    free(output);
    workspace_close(&workspace);
    return failures == 0;
}
#endif

static int case_frames_export(void)
{
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    const char *arguments[] = {
        "--frame-size", "512", "--hop-size", "256",
        "export", NULL, "--kind", "frames", "--output", NULL
    };
    char *output;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    CHECK(join_path(output_path, sizeof(output_path),
                    workspace.directory, "frames.csv"),
          "frames CSV path is too long");
    arguments[5] = workspace.input;
    arguments[9] = output_path;
    status = run_analyzer(&workspace, arguments, 10U, NULL);
    expect_success(status, &workspace);
    output = read_file(output_path);
    CHECK(output != NULL &&
              strncmp(output,
                      "time_seconds,rms_dbfs,frame_lufs,pitch_hz,"
                      "pitch_confidence,onset_strength,spectral_centroid_hz,"
                      "spectral_rolloff_85_hz,spectral_flatness",
                      strlen("time_seconds,rms_dbfs,frame_lufs,pitch_hz,"
                             "pitch_confidence,onset_strength,"
                             "spectral_centroid_hz,spectral_rolloff_85_hz,"
                             "spectral_flatness")) == 0,
          "frames export must use the versioned Stage 1 columns");
    CHECK(output != NULL && strstr(output, ",band_9_db\n") != NULL,
          "frames export must include all ten band columns");
    CHECK(output != NULL && line_count(output) > 100,
          "frames export must contain time rows");
    free(output);
    (void)remove_file(output_path);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_spectrogram_export(void)
{
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    const char *arguments[] = {
        "--frame-size", "256", "--hop-size", "256",
        "export", NULL, "--kind", "spectrogram", "--output", NULL
    };
    char *output;
    const char *first_row;
    const char *second_row;
    double time1 = 0.0;
    double frequency1 = -1.0;
    double power1 = 0.0;
    double time2 = 0.0;
    double frequency2 = 0.0;
    double power2 = 0.0;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    CHECK(join_path(output_path, sizeof(output_path),
                    workspace.directory, "spectrogram.csv"),
          "spectrogram CSV path is too long");
    arguments[5] = workspace.input;
    arguments[9] = output_path;
    status = run_analyzer(&workspace, arguments, 10U, NULL);
    expect_success(status, &workspace);
    output = read_file(output_path);
    CHECK(output != NULL &&
              strncmp(output, "time_seconds,frequency_hz,power_db\n", 35U) == 0,
          "spectrogram export must use three stable columns");
    first_row = output != NULL ? strchr(output, '\n') : NULL;
    first_row = first_row != NULL ? first_row + 1U : NULL;
    second_row = first_row != NULL ? strchr(first_row, '\n') : NULL;
    second_row = second_row != NULL ? second_row + 1U : NULL;
    CHECK(first_row != NULL &&
              sscanf(first_row, "%lf,%lf,%lf", &time1, &frequency1, &power1) == 3,
          "spectrogram first row must contain three numbers");
    CHECK(second_row != NULL &&
              sscanf(second_row, "%lf,%lf,%lf", &time2, &frequency2, &power2) == 3,
          "spectrogram second row must contain three numbers");
    CHECK(near(time1, time2, 1e-15) && near(frequency1, 0.0, 1e-15) &&
              near(frequency2, 31.25, 1e-12),
          "spectrogram bins must follow sample_rate/frame_size");
    CHECK(output != NULL && line_count(output) > 10000,
          "spectrogram export must contain the full time-frequency grid");
    free(output);
    (void)remove_file(output_path);
    workspace_close(&workspace);
    return failures == 0;
}

static int case_output_safety(void)
{
    static const char sentinel[] = "keep this file\n";
    TestWorkspace workspace;
    char output_path[PATH_MAX];
#if !defined(_WIN32)
    char hardlink_path[PATH_MAX];
    struct stat output_status;
#endif
    const char *without_replace[] = {
        "export", NULL, "--kind", "frames", "--output", NULL
    };
    const char *with_replace[] = {
        "--replace", "export", NULL, "--kind", "frames", "--output", NULL
    };
    const char *same_input[] = {
        "--replace", "export", NULL, "--kind", "frames", "--output", NULL
    };
    char *output;
    uint64_t input_hash;
    uint64_t after_hash;
    uint64_t sentinel_hash;
    uint64_t refused_hash;
    int hash_ok;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    CHECK(join_path(output_path, sizeof(output_path),
                    workspace.directory, "existing.csv"),
          "output path is too long");
    CHECK(write_text(output_path, sentinel),
          "could not create the existing output fixture");
#if !defined(_WIN32)
    CHECK(chmod(output_path, 0600) == 0,
          "could not set the existing output mode to 0600");
    CHECK(stat(output_path, &output_status) == 0 &&
              (output_status.st_mode & 0777) == 0600,
          "existing output must start with mode 0600");
#endif
    sentinel_hash = hash_file(output_path, &hash_ok);
    CHECK(hash_ok, "could not hash existing output");
    without_replace[1] = workspace.input;
    without_replace[5] = output_path;
    status = run_analyzer(&workspace, without_replace, 6U, NULL);
    expect_data_failure(status, &workspace, "cannot create output");
    refused_hash = hash_file(output_path, &hash_ok);
    CHECK(hash_ok && refused_hash == sentinel_hash,
          "refused overwrite must preserve the existing output");

    with_replace[2] = workspace.input;
    with_replace[6] = output_path;
    status = run_analyzer(&workspace, with_replace, 7U, NULL);
    expect_success(status, &workspace);
    output = read_file(output_path);
    CHECK(output != NULL && strncmp(output, "time_seconds,", 13U) == 0,
          "--replace must replace an existing regular output");
    free(output);
#if !defined(_WIN32)
    CHECK(stat(output_path, &output_status) == 0 &&
              (output_status.st_mode & 0777) == 0600,
          "--replace must keep the existing output mode 0600");
#endif

    input_hash = hash_file(workspace.input, &hash_ok);
    CHECK(hash_ok, "could not hash input before same-file test");
    same_input[2] = workspace.input;
    same_input[6] = workspace.input;
    status = run_analyzer(&workspace, same_input, 7U, NULL);
    expect_data_failure(status, &workspace, "resolves to the input");
    after_hash = hash_file(workspace.input, &hash_ok);
    CHECK(hash_ok && after_hash == input_hash,
          "same-path output refusal must preserve input bytes");

#if !defined(_WIN32)
    CHECK(join_path(hardlink_path, sizeof(hardlink_path),
                    workspace.directory, "input-hardlink.csv"),
          "hard-link path is too long");
    CHECK(link(workspace.input, hardlink_path) == 0,
          "could not create a hard link for the safety test");
    if (failures == 0) {
        same_input[6] = hardlink_path;
        status = run_analyzer(&workspace, same_input, 7U, NULL);
        expect_data_failure(status, &workspace, "resolves to the input");
        after_hash = hash_file(workspace.input, &hash_ok);
        CHECK(hash_ok && after_hash == input_hash,
              "hard-linked output refusal must preserve input bytes");
    }
    (void)remove_file(hardlink_path);
#endif
    (void)remove_file(output_path);
    workspace_close(&workspace);
    return failures == 0;
}

#if !defined(_WIN32)
static int case_replace_write_failure(void)
{
    static const char sentinel[] = "keep this file through write failure\n";
    TestWorkspace workspace;
    char output_path[PATH_MAX];
    const char *arguments[] = {
        "--replace", "export", NULL, "--kind", "frames", "--output", NULL
    };
    uint64_t before_hash;
    uint64_t after_hash;
    int hash_ok;
    int status;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    CHECK(join_path(output_path, sizeof(output_path),
                    workspace.directory, "write-failure.csv"),
          "write-failure output path is too long");
    CHECK(write_text(output_path, sentinel),
          "could not create the replacement failure fixture");
    before_hash = hash_file(output_path, &hash_ok);
    CHECK(hash_ok, "could not hash replacement fixture before failure");
    arguments[2] = workspace.input;
    arguments[6] = output_path;
    status = run_analyzer_with_file_limit(
        &workspace, arguments, 7U, (rlim_t)512U);
    expect_data_failure(status, &workspace, "CSV output");
    after_hash = hash_file(output_path, &hash_ok);
    CHECK(hash_ok && after_hash == before_hash,
          "failed replacement must preserve every byte of the old file");
    CHECK(temporary_output_count(workspace.directory) == 0,
          "failed replacement must remove its .hwa-tmp-* file");
    (void)remove_file(output_path);
    workspace_close(&workspace);
    return failures == 0;
}
#endif

typedef struct InvariantMetrics {
    double peak;
    double true_peak;
    double rms;
    double integrated_lufs;
    double centroid_hz;
    double silence_fraction;
    double correlation;
} InvariantMetrics;

static int read_invariant_metrics(const char *json, InvariantMetrics *metrics)
{
    return json_number(json, "peak", &metrics->peak) &&
           json_number(json, "true_peak", &metrics->true_peak) &&
           json_number(json, "rms", &metrics->rms) &&
           json_number(json, "integrated_lufs", &metrics->integrated_lufs) &&
           json_number(json, "centroid_hz", &metrics->centroid_hz) &&
           json_number(json, "silence_fraction", &metrics->silence_fraction) &&
           json_number(json, "correlation", &metrics->correlation);
}

static int metrics_near(const InvariantMetrics *left,
                        const InvariantMetrics *right)
{
    return near(left->peak, right->peak, 1e-12) &&
           near(left->true_peak, right->true_peak, 1e-12) &&
           near(left->rms, right->rms, 1e-12) &&
           near(left->integrated_lufs, right->integrated_lufs, 1e-9) &&
           near(left->centroid_hz, right->centroid_hz, 1e-9) &&
           near(left->silence_fraction, right->silence_fraction, 1e-12) &&
           near(left->correlation, right->correlation, 1e-12);
}

static int case_block_invariance(void)
{
    static const char *blocks[] = {"1", "257", "4096"};
    TestWorkspace workspace;
    InvariantMetrics metrics[3];
    size_t index;

    if (!setup_fixture(&workspace)) {
        return 0;
    }
    memset(metrics, 0, sizeof(metrics));
    for (index = 0U; index < 3U; ++index) {
        const char *arguments[] = {
            "--json", "--block-frames", blocks[index], "inspect", workspace.input
        };
        char *output;
        int status = run_analyzer(&workspace, arguments, 5U, NULL);

        expect_success(status, &workspace);
        output = read_file(workspace.standard_output);
        CHECK(output != NULL && read_invariant_metrics(output, &metrics[index]),
              "block %s output lacks an invariant metric", blocks[index]);
        free(output);
    }
    CHECK(metrics_near(&metrics[0], &metrics[1]) &&
              metrics_near(&metrics[0], &metrics[2]),
          "block sizes 1, 257, and 4096 changed results beyond "
          "1e-12 levels/correlation, 1e-9 LUFS/Hz");
    workspace_close(&workspace);
    return failures == 0;
}

static const TestCase test_cases[] = {
    {"stage1-schema", case_schema},
    {"stage1-channels", case_channels},
    {"stage1-limits", case_limits},
#if !defined(_WIN32)
    {"stage1-stdin", case_stdin},
#endif
    {"stage1-frames-export", case_frames_export},
    {"stage1-spectrogram-export", case_spectrogram_export},
    {"stage1-output-safety", case_output_safety},
#if !defined(_WIN32)
    {"stage1-replace-write-failure", case_replace_write_failure},
#endif
    {"stage1-block-invariance", case_block_invariance}
};

static void print_usage(const char *program)
{
    size_t index;

    (void)fprintf(stderr, "usage: %s ANALYZER CASE\ncases:", program);
    for (index = 0U;
         index < sizeof(test_cases) / sizeof(test_cases[0]); ++index) {
        (void)fprintf(stderr, " %s", test_cases[index].name);
    }
    (void)fputs(" all\n", stderr);
}

int main(int argc, char **argv)
{
    size_t index;
    int matched = 0;

    if (argc != 3) {
        print_usage(argv[0]);
        return 2;
    }
    analyzer_path = argv[1];
    for (index = 0U;
         index < sizeof(test_cases) / sizeof(test_cases[0]); ++index) {
        if (strcmp(argv[2], "all") == 0 ||
            strcmp(argv[2], test_cases[index].name) == 0) {
            matched = 1;
            failures = 0;
            if (!test_cases[index].function() || failures != 0) {
                (void)fprintf(stderr, "case %s failed with %d assertion(s)\n",
                              test_cases[index].name, failures);
                return 1;
            }
            (void)printf("PASS %s\n", test_cases[index].name);
        }
    }
    if (!matched) {
        print_usage(argv[0]);
        return 2;
    }
    return 0;
}
