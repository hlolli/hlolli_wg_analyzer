#ifndef HWA_WINDOWS_TEST_PROCESS_H
#define HWA_WINDOWS_TEST_PROCESS_H

#if defined(_WIN32)

#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static inline int hwa_test_spawn_redirected(
    const char *program,
    const char *const *arguments,
    size_t argument_count,
    const char *input_path,
    const char *output_path,
    const char *error_path)
{
    const char **arguments_with_program;
    intptr_t status;
    size_t index;
    int saved_input = -1;
    int saved_output = -1;
    int saved_error = -1;
    int input = -1;
    int output = -1;
    int errors = -1;
    int result = -1;
    int redirected = 0;
    if (program == NULL || output_path == NULL || error_path == NULL ||
        argument_count > (SIZE_MAX / sizeof(*arguments_with_program)) - 2U)
        return -1;
    arguments_with_program = (const char **)calloc(
        argument_count + 2U, sizeof(*arguments_with_program));
    if (arguments_with_program == NULL) return -1;
    arguments_with_program[0] = program;
    for (index = 0U; index < argument_count; ++index)
        arguments_with_program[index + 1U] = arguments[index];
    (void)fflush(stdout);
    (void)fflush(stderr);
    if (input_path != NULL) {
        saved_input = _dup(_fileno(stdin));
        input = _open(input_path, _O_RDONLY | _O_BINARY);
    }
    saved_output = _dup(_fileno(stdout));
    saved_error = _dup(_fileno(stderr));
    output = _open(output_path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
    errors = _open(error_path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
    if ((input_path != NULL && (saved_input < 0 || input < 0)) ||
        saved_output < 0 || saved_error < 0 || output < 0 || errors < 0) {
        goto cleanup;
    }
    if ((input_path != NULL && _dup2(input, _fileno(stdin)) != 0) ||
        _dup2(output, _fileno(stdout)) != 0 ||
        _dup2(errors, _fileno(stderr)) != 0) goto restore;
    redirected = 1;
    status = _spawnv(_P_WAIT, program, arguments_with_program);
    if (status >= 0 && status <= INT_MAX) result = (int)status;

restore:
    if (redirected) {
        (void)fflush(stdout);
        (void)fflush(stderr);
    }
    if (saved_input >= 0) (void)_dup2(saved_input, _fileno(stdin));
    if (saved_output >= 0) (void)_dup2(saved_output, _fileno(stdout));
    if (saved_error >= 0) (void)_dup2(saved_error, _fileno(stderr));
cleanup:
    if (input >= 0) (void)_close(input);
    if (output >= 0) (void)_close(output);
    if (errors >= 0) (void)_close(errors);
    if (saved_input >= 0) (void)_close(saved_input);
    if (saved_output >= 0) (void)_close(saved_output);
    if (saved_error >= 0) (void)_close(saved_error);
    free(arguments_with_program);
    return result;
}

#endif

#endif
