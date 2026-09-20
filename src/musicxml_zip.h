#ifndef HWA_MUSICXML_ZIP_H
#define HWA_MUSICXML_ZIP_H
#include <stddef.h>
#include <stdint.h>

/* Read one ZIP member into memory. Never opens or creates a filesystem path. */
int hwa_musicxml_zip_member(const unsigned char *zip, size_t size, const char *name,
                           uint64_t limit, unsigned char **data, size_t *length,
                           char *error, size_t error_size);
#endif
