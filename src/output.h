#ifndef HWA_OUTPUT_H
#define HWA_OUTPUT_H

#include <stdio.h>

int hwa_json_write_string(FILE *stream, const char *text);
int hwa_json_write_byte_hex(FILE *stream, const char *text);

#endif
