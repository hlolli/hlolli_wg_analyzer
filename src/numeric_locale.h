#ifndef HWA_NUMERIC_LOCALE_H
#define HWA_NUMERIC_LOCALE_H

#include <stddef.h>

typedef struct {
    void *locale;
    void *previous;
    char *saved_locale;
    int previous_mode;
    int active;
} HWANumericLocale;

int hwa_c_numeric_locale_begin(HWANumericLocale *scope);
int hwa_c_numeric_locale_end(HWANumericLocale *scope);
int hwa_c_locale_format_double(const HWANumericLocale *scope,
                               char *buffer,
                               size_t buffer_size,
                               double value);
int hwa_c_locale_parse_double(const HWANumericLocale *scope,
                              const char *text,
                              double *value);

#endif
