#ifndef _WIN32
#if defined(_XOPEN_SOURCE) && _XOPEN_SOURCE < 700
#undef _XOPEN_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#include "numeric_locale.h"

#include <errno.h>
#include <locale.h>
#if defined(__APPLE__) && !defined(_WIN32)
#include <xlocale.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hwa_c_numeric_locale_begin(HWANumericLocale *scope)
{
    if (scope == NULL) return -1;
    memset(scope, 0, sizeof(*scope));
#ifdef _WIN32
    {
        const char *current;
        size_t length;
        scope->previous_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        if (scope->previous_mode == -1) return -1;
        current = setlocale(LC_NUMERIC, NULL);
        if (current == NULL || strlen(current) == SIZE_MAX) goto windows_error;
        length = strlen(current);
        scope->saved_locale = (char *)malloc(length + 1U);
        if (scope->saved_locale == NULL) goto windows_error;
        memcpy(scope->saved_locale, current, length + 1U);
        if (setlocale(LC_NUMERIC, "C") == NULL) goto windows_error;
        scope->locale = (void *)_create_locale(LC_NUMERIC, "C");
        if (scope->locale == NULL) goto windows_restore_error;
    }
#else
    {
        locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
        locale_t previous;
        if (locale == (locale_t)0) return -1;
        previous = uselocale(locale);
        if (previous == (locale_t)0) {
            freelocale(locale);
            return -1;
        }
        scope->locale = (void *)locale;
        scope->previous = (void *)previous;
    }
#endif
    scope->active = 1;
    return 0;
#ifdef _WIN32
windows_restore_error:
    (void)setlocale(LC_NUMERIC, scope->saved_locale);
windows_error:
    free(scope->saved_locale);
    scope->saved_locale = NULL;
    (void)_configthreadlocale(scope->previous_mode);
    return -1;
#endif
}

int hwa_c_numeric_locale_end(HWANumericLocale *scope)
{
    if (scope == NULL || !scope->active || scope->locale == NULL) return -1;
#ifdef _WIN32
    int result = 0;
    if (scope->saved_locale == NULL ||
        setlocale(LC_NUMERIC, scope->saved_locale) == NULL) result = -1;
    _free_locale((_locale_t)scope->locale);
    free(scope->saved_locale);
    if (_configthreadlocale(scope->previous_mode) == -1) result = -1;
    memset(scope, 0, sizeof(*scope));
    return result;
#else
    if (uselocale((locale_t)scope->previous) == (locale_t)0) return -1;
    freelocale((locale_t)scope->locale);
    memset(scope, 0, sizeof(*scope));
    return 0;
#endif
}

int hwa_c_locale_format_double(const HWANumericLocale *scope,
                               char *buffer,
                               size_t buffer_size,
                               double value)
{
    int length;
    if (scope == NULL || !scope->active || scope->locale == NULL ||
        buffer == NULL || buffer_size == 0U || !isfinite(value)) return -1;
    if (value == 0.0) value = 0.0;
#ifdef _WIN32
    length = _snprintf_l(buffer, buffer_size, "%.17g",
                         (_locale_t)scope->locale, value);
#else
    length = snprintf(buffer, buffer_size, "%.17g", value);
#endif
    if (length < 0 || (size_t)length >= buffer_size) return -1;
    return 0;
}

int hwa_c_locale_parse_double(const HWANumericLocale *scope,
                              const char *text,
                              double *value)
{
    char *end = NULL;
    double parsed;
    if (scope == NULL || !scope->active || scope->locale == NULL ||
        text == NULL || text[0] == '\0' || value == NULL) return -1;
    errno = 0;
#ifdef _WIN32
    parsed = _strtod_l(text, &end, (_locale_t)scope->locale);
#else
    parsed = strtod(text, &end);
#endif
    if (end == text || *end != '\0' || !isfinite(parsed) ||
        (errno == ERANGE && fpclassify(parsed) != FP_SUBNORMAL)) return -1;
    *value = parsed == 0.0 ? 0.0 : parsed;
    return 0;
}
