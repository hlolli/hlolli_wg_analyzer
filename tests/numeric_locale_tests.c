#include "numeric_locale.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int hwa_test_set_comma_locale(void)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8",
        "fr_FR.UTF-8",
        "de_DE",
        "German_Germany.1252",
        "de-DE"
    };
    size_t index;
    for (index = 0U;
         index < sizeof(candidates) / sizeof(candidates[0]);
         index++) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL &&
            strcmp(localeconv()->decimal_point, ",") == 0) return 0;
    }
    return -1;
}

int main(void)
{
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    char text[64];
    double parsed = 0.0;
    HWANumericLocale scope;
    HWANumericLocale nested;
    int failed = 0;
    if (current == NULL || strlen(current) >= sizeof(saved)) return 1;
    memcpy(saved, current, strlen(current) + 1U);
    if (hwa_test_set_comma_locale() != 0) {
        (void)setlocale(LC_NUMERIC, saved);
        return 77;
    }
    if (hwa_c_numeric_locale_begin(&scope) != 0) {
        (void)setlocale(LC_NUMERIC, saved);
        return 1;
    }
    if (hwa_c_locale_format_double(
            &scope, text, sizeof(text), 1234.5) != 0 ||
        strcmp(text, "1234.5") != 0) {
        fputs("C-locale double formatting failed\n", stderr);
        failed = 1;
    }
    if (hwa_c_locale_parse_double(&scope, "1234.5", &parsed) != 0 ||
        fabs(parsed - 1234.5) > 1e-12) {
        fputs("C-locale double parsing failed\n", stderr);
        failed = 1;
    }
    if (hwa_c_locale_parse_double(&scope, "1234,5", &parsed) == 0) {
        fputs("comma decimal input was accepted\n", stderr);
        failed = 1;
    }
#ifdef DBL_TRUE_MIN
    if (hwa_c_locale_format_double(
            &scope, text, sizeof(text), DBL_TRUE_MIN) != 0 ||
        hwa_c_locale_parse_double(&scope, text, &parsed) != 0 ||
        parsed != DBL_TRUE_MIN) {
        fputs("finite subnormal double did not round trip\n", stderr);
        failed = 1;
    }
#endif
    if (hwa_c_numeric_locale_begin(&nested) != 0 ||
        strcmp(localeconv()->decimal_point, ".") != 0 ||
        hwa_c_numeric_locale_end(&nested) != 0 ||
        strcmp(localeconv()->decimal_point, ".") != 0) {
        fputs("nested C-locale scope failed\n", stderr);
        failed = 1;
    }
    if (hwa_c_numeric_locale_end(&scope) != 0) {
        fputs("cannot restore the caller's numeric locale\n", stderr);
        failed = 1;
    }
    if (strcmp(localeconv()->decimal_point, ",") != 0) {
        fputs("helper changed the caller's numeric locale\n", stderr);
        failed = 1;
    }
    if (setlocale(LC_NUMERIC, saved) == NULL) return 1;
    return failed;
}
