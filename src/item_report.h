#ifndef HWA_ITEM_REPORT_H
#define HWA_ITEM_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_items_json(FILE *stream, const HWAItemSet *items);
int hwa_report_items_text(FILE *stream, const HWAItemSet *items);

#endif
