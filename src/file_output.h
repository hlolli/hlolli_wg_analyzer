#ifndef HWA_FILE_OUTPUT_H
#define HWA_FILE_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct HWAFileOutput {
    FILE *stream;
    char *path;
    char *temporary_path;
    uint64_t replacement_device;
    uint64_t replacement_file;
    uint64_t created_device;
    uint64_t created_file;
    int direct_path_created;
    int created_identity_valid;
    int replacement_existed;
    int replacement_mode;
    int standard_output;
} HWAFileOutput;

/*
 * Open a new output or a same-directory replacement temporary file. Every
 * named protected input is checked by file identity, so a hard link cannot be
 * used as an output. A path of "-" writes to standard output and cannot be
 * used with replace. On Windows, "-" keeps the process-owned stdout descriptor
 * in binary mode so canonical CRLF bytes are not expanded by the C runtime.
 */
int hwa_file_output_open(HWAFileOutput *output,
                         const char *path,
                         const char *const *protected_paths,
                         size_t protected_path_count,
                         int replace,
                         char *error,
                         size_t error_size);

/* Return the stream owned by a successful open. */
FILE *hwa_file_output_stream(HWAFileOutput *output);

/*
 * Flush, close, and commit the output. On a write fault this removes a new
 * partial file or replacement temporary file if its identity still matches.
 * A changed path is left alone. Standard output is flushed, not closed.
 */
int hwa_file_output_finish(HWAFileOutput *output,
                           const char *output_name,
                           char *error,
                           size_t error_size);

/*
 * Close and remove an uncommitted file if its path still names the file made
 * by open. Return -1 and leave changed paths alone after an identity mismatch.
 * Safe after a failed open or finish.
 */
int hwa_file_output_abort(HWAFileOutput *output);

#if defined(HWA_FILE_OUTPUT_TESTING)
/* Fail the next created-file identity reads in the focused unit test. */
void hwa_file_output_test_fail_created_identity(unsigned count);
/* Exercise standard-output binary-mode success and failure paths. */
void hwa_file_output_test_fail_stdout_binary(unsigned count);
unsigned hwa_file_output_test_stdout_binary_calls(void);
#endif

#endif
