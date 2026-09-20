#include "internal.h"
#include "musicxml_zip.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A bounded XML tree. Index zero means absent; no external resource loader. */
typedef struct XMLNode {
    const char *name;
    const char *text;
    size_t first, last, next;
    size_t start, end;
    int attribute;
} XMLNode;

typedef struct XMLMeasure {
    double length, start;
    size_t head, tail, ending_end;
    unsigned repeat_start, repeat_times, ending_mask, ending_stop, visits;
    const char *segno, *coda, *dalsegno, *tocoda;
    int dacapo, fine;
    double jump_at, initial_tempo;
} XMLMeasure;

typedef struct XMLReader {
    const unsigned char *data;
    size_t size, pos;
    char *strings;
    size_t strings_size, strings_used;
    XMLNode *nodes;
    size_t count, capacity;
    XMLMeasure *measures;
    size_t measure_count, measure_capacity;
    size_t *event_measures;
    size_t event_capacity;
    unsigned pedal_cc[17];
    uint64_t work;
    HWAMusicXMLOptions limits;
    HWAMusicXMLScore *score;
    char *error;
    size_t error_size;
} XMLReader;

static int fail(XMLReader *r, const char *message)
{
    hwa_set_error(r->error, r->error_size, "MusicXML at byte %zu: %s", r->pos, message);
    return -1;
}

static int space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_space(XMLReader *r)
{
    while (r->pos < r->size && space(r->data[r->pos])) r->pos++;
}

static int at(const XMLReader *r, const char *s)
{
    size_t n = strlen(s);
    return n <= r->size-r->pos && memcmp(r->data+r->pos, s, n) == 0;
}

static int codepoint(uint32_t c)
{
    return c == 9U || c == 10U || c == 13U ||
        (c >= 32U && c <= 0xd7ffU) || (c >= 0xe000U && c <= 0xfffdU) ||
        (c >= 0x10000U && c <= 0x10ffffU);
}

static int utf8(const unsigned char *s, size_t n)
{
    size_t i = 0U;
    while (i < n) {
        uint32_t c = s[i++], minimum = 0U;
        unsigned extra = 0U;
        if (c >= 0xc2U && c <= 0xdfU) { c &= 31U; extra = 1U; minimum = 0x80U; }
        else if (c >= 0xe0U && c <= 0xefU) { c &= 15U; extra = 2U; minimum = 0x800U; }
        else if (c >= 0xf0U && c <= 0xf4U) { c &= 7U; extra = 3U; minimum = 0x10000U; }
        else if (c >= 0x80U) return -1;
        while (extra != 0U) {
            if (i == n || (s[i] & 0xc0U) != 0x80U) return -1;
            c = (c << 6U) | (uint32_t)(s[i++] & 63U);
            extra--;
        }
        if (c < minimum || !codepoint(c)) return -1;
    }
    return 0;
}

static size_t encode(char *out, uint32_t c)
{
    if (c < 0x80U) { out[0] = (char)c; return 1U; }
    if (c < 0x800U) {
        out[0] = (char)(0xc0U | (c >> 6U)); out[1] = (char)(0x80U | (c & 63U));
        return 2U;
    }
    if (c < 0x10000U) {
        out[0] = (char)(0xe0U | (c >> 12U)); out[1] = (char)(0x80U | ((c >> 6U) & 63U));
        out[2] = (char)(0x80U | (c & 63U)); return 3U;
    }
    out[0] = (char)(0xf0U | (c >> 18U)); out[1] = (char)(0x80U | ((c >> 12U) & 63U));
    out[2] = (char)(0x80U | ((c >> 6U) & 63U)); out[3] = (char)(0x80U | (c & 63U));
    return 4U;
}

static const char *text(XMLReader *r, size_t begin, size_t end, int entities)
{
    char *out;
    size_t length = 0U, i = begin;
    if (end-begin >= r->strings_size-r->strings_used) {
        (void)fail(r, "string storage limit exceeded"); return NULL;
    }
    out = r->strings+r->strings_used;
    while (i < end) {
        unsigned char c = r->data[i++];
        if (c == '&' && entities) {
            size_t start = i;
            uint32_t value = 0U;
            while (i < end && r->data[i] != ';' && i-start < 32U) i++;
            if (i == end || r->data[i] != ';') goto bad_entity;
            if (i-start == 3U && memcmp(r->data+start, "amp", 3U) == 0) value = '&';
            else if (i-start == 2U && memcmp(r->data+start, "lt", 2U) == 0) value = '<';
            else if (i-start == 2U && memcmp(r->data+start, "gt", 2U) == 0) value = '>';
            else if (i-start == 4U && memcmp(r->data+start, "quot", 4U) == 0) value = '"';
            else if (i-start == 4U && memcmp(r->data+start, "apos", 4U) == 0) value = '\'';
            else if (r->data[start] == '#') {
                unsigned base = 10U;
                size_t digit = start+1U;
                if (digit < i && r->data[digit] == 'x') { base = 16U; digit++; }
                if (digit == i) goto bad_entity;
                for (; digit < i; digit++) {
                    unsigned d = r->data[digit];
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d = d-'a'+10U;
                    else if (d >= 'A' && d <= 'F') d = d-'A'+10U;
                    else goto bad_entity;
                    if (d >= base || value > (0x10ffffU-d)/base) goto bad_entity;
                    value = value*base+d;
                }
            } else goto bad_entity;
            if (!codepoint(value)) goto bad_entity;
            length += encode(out+length, value);
            i++;
        } else {
            if (c == '\r') { c = '\n'; if (i < end && r->data[i] == '\n') i++; }
            out[length++] = (char)c;
        }
    }
    out[length] = '\0';
    r->strings_used += length+1U;
    return out;
bad_entity:
    (void)fail(r, "invalid or unsupported XML entity");
    return NULL;
}

static void *grow(XMLReader *r, void *old, size_t old_count, size_t count, size_t unit)
{
    void *result;
    uint64_t bytes;
    if (count > SIZE_MAX/unit) { (void)fail(r, "allocation overflow"); return NULL; }
    bytes = (uint64_t)(count*unit);
    if (bytes > r->limits.max_work_bytes-r->work) {
        (void)fail(r, "work byte limit exceeded"); return NULL;
    }
    result = realloc(old, count*unit);
    if (result == NULL) { (void)fail(r, "out of memory"); return NULL; }
    r->work += bytes-(uint64_t)(old_count*unit);
    return result;
}

static size_t node(XMLReader *r, size_t parent, int attribute)
{
    size_t index;
    if (r->count == r->limits.max_nodes) { (void)fail(r, "XML node limit exceeded"); return 0U; }
    if (r->count+1U >= r->capacity) {
        size_t cap = r->capacity == 0U ? 64U : r->capacity*2U;
        XMLNode *p;
        if (cap < r->capacity || cap > r->limits.max_nodes+1U) cap = r->limits.max_nodes+1U;
        p = grow(r, r->nodes, r->capacity, cap, sizeof(*p));
        if (p == NULL) return 0U;
        r->nodes = p; r->capacity = cap;
    }
    index = ++r->count;
    memset(&r->nodes[index], 0, sizeof(r->nodes[index]));
    r->nodes[index].text = "";
    r->nodes[index].attribute = attribute;
    if (parent != 0U) {
        if (r->nodes[parent].last != 0U) r->nodes[r->nodes[parent].last].next = index;
        else r->nodes[parent].first = index;
        r->nodes[parent].last = index;
    }
    return index;
}

static const char *name(XMLReader *r)
{
    size_t start = r->pos;
    while (r->pos < r->size) {
        unsigned char c = r->data[r->pos];
        int letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && !(r->pos > start && ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':'))) break;
        r->pos++;
    }
    if (r->pos == start) { (void)fail(r, "invalid XML name"); return NULL; }
    return text(r, start, r->pos, 0);
}

static int skip_misc(XMLReader *r)
{
    if (at(r, "<!--")) {
        r->pos += 4U;
        while (r->pos < r->size && !at(r, "--")) r->pos++;
        if (!at(r, "-->")) return fail(r, "invalid XML comment");
        r->pos += 3U; return 1;
    }
    if (at(r, "<?")) {
        size_t begin = r->pos;
        const char *target;
        r->pos += 2U;
        target = name(r);
        if (target == NULL) return -1;
        if ((target[0] == 'x' || target[0] == 'X') &&
            (target[1] == 'm' || target[1] == 'M') &&
            (target[2] == 'l' || target[2] == 'L') && target[3] == '\0') {
            int stage = 0;
            if (strcmp(target, "xml") != 0 ||
                (begin != 0U && !(begin == 3U && memcmp(r->data, "\xef\xbb\xbf", 3U) == 0)))
                return fail(r, "misplaced XML declaration");
            for (;;) {
                size_t before = r->pos, start;
                const char *key, *value;
                unsigned char quote;
                skip_space(r);
                if (at(r, "?>")) {
                    if (stage == 0) return fail(r, "missing XML version");
                    r->pos += 2U; return 1;
                }
                if (before == r->pos) return fail(r, "invalid XML declaration separator");
                key = name(r);
                if (key == NULL) return -1;
                skip_space(r);
                if (!at(r, "=")) return fail(r, "invalid XML declaration value");
                r->pos++; skip_space(r);
                if (r->pos == r->size || (r->data[r->pos] != '\'' && r->data[r->pos] != '"'))
                    return fail(r, "unquoted XML declaration value");
                quote = r->data[r->pos++]; start = r->pos;
                while (r->pos < r->size && r->data[r->pos] != quote) r->pos++;
                if (r->pos == r->size) return fail(r, "unterminated XML declaration");
                value = text(r, start, r->pos, 0); r->pos++;
                if (value == NULL) return -1;
                if (strcmp(key, "version") == 0 && stage == 0 && strcmp(value, "1.0") == 0) stage = 1;
                else if (strcmp(key, "encoding") == 0 && stage == 1) {
                    if (strlen(value) != 5U || (value[0] != 'U' && value[0] != 'u') ||
                        (value[1] != 'T' && value[1] != 't') || (value[2] != 'F' && value[2] != 'f') ||
                        value[3] != '-' || value[4] != '8') return fail(r, "only UTF-8 XML is supported");
                    stage = 2;
                } else if (strcmp(key, "standalone") == 0 && (stage == 1 || stage == 2) &&
                           (strcmp(value, "yes") == 0 || strcmp(value, "no") == 0)) stage = 3;
                else return fail(r, "invalid or unsupported XML declaration");
            }
        }
        if (!at(r, "?>") && (r->pos == r->size || !space(r->data[r->pos])))
            return fail(r, "invalid processing instruction");
        while (r->pos < r->size && !at(r, "?>")) r->pos++;
        if (!at(r, "?>")) return fail(r, "unterminated processing instruction");
        r->pos += 2U; return 1;
    }
    return 0;
}

static size_t element(XMLReader *r, size_t parent, unsigned depth)
{
    size_t index, begin = r->pos, attribute_count = 0U;
    const char *tag;
    if (depth > 64U) { (void)fail(r, "XML depth limit exceeded"); return 0U; }
    r->pos++;
    tag = name(r);
    if (tag == NULL || (index = node(r, parent, 0)) == 0U) return 0U;
    r->nodes[index].name = tag; r->nodes[index].start = begin;
    for (;;) {
        size_t before = r->pos, attr, start, scan;
        const char *key, *value;
        unsigned char quote;
        skip_space(r);
        if (at(r, "/>")) { r->pos += 2U; r->nodes[index].end = r->pos; return index; }
        if (at(r, ">")) { r->pos++; break; }
        if (before == r->pos) { (void)fail(r, "expected attribute separator"); return 0U; }
        if (++attribute_count > 128U) { (void)fail(r, "attribute limit exceeded"); return 0U; }
        key = name(r);
        if (key == NULL) return 0U;
        for (scan = r->nodes[index].first; scan != 0U; scan = r->nodes[scan].next)
            if (strcmp(r->nodes[scan].name, key) == 0) { (void)fail(r, "duplicate XML attribute"); return 0U; }
        skip_space(r);
        if (!at(r, "=")) { (void)fail(r, "expected attribute value"); return 0U; }
        r->pos++; skip_space(r);
        if (r->pos == r->size || (r->data[r->pos] != '\'' && r->data[r->pos] != '"')) {
            (void)fail(r, "expected quoted attribute"); return 0U;
        }
        quote = r->data[r->pos++]; start = r->pos;
        while (r->pos < r->size && r->data[r->pos] != quote && r->data[r->pos] != '<') r->pos++;
        if (r->pos == r->size || r->data[r->pos] != quote) { (void)fail(r, "invalid attribute"); return 0U; }
        value = text(r, start, r->pos, 1); r->pos++;
        if (value == NULL || (attr = node(r, index, 1)) == 0U) return 0U;
        r->nodes[attr].name = key; r->nodes[attr].text = value;
    }
    while (r->pos < r->size) {
        int misc;
        if (at(r, "</")) {
            const char *close;
            r->pos += 2U; close = name(r); skip_space(r);
            if (close == NULL || strcmp(close, tag) != 0 || !at(r, ">")) {
                (void)fail(r, "mismatched closing element"); return 0U;
            }
            r->pos++; r->nodes[index].end = r->pos; return index;
        }
        misc = skip_misc(r);
        if (misc < 0) return 0U;
        if (misc != 0) continue;
        if (at(r, "<![CDATA[")) {
            const char *value;
            r->pos += 9U; begin = r->pos;
            while (r->pos < r->size && !at(r, "]]>")) r->pos++;
            if (!at(r, "]]>")) { (void)fail(r, "unterminated CDATA"); return 0U; }
            value = text(r, begin, r->pos, 0); r->pos += 3U;
            if (value == NULL) return 0U;
            if (r->nodes[index].text[0] != '\0') { (void)fail(r, "split text is unsupported"); return 0U; }
            r->nodes[index].text = value;
        } else if (at(r, "<")) {
            if (element(r, index, depth+1U) == 0U) return 0U;
        } else {
            const char *value;
            begin = r->pos;
            while (r->pos < r->size && r->data[r->pos] != '<') {
                if (at(r, "]]>")) { (void)fail(r, "invalid text delimiter"); return 0U; }
                r->pos++;
            }
            value = text(r, begin, r->pos, 1);
            if (value == NULL) return 0U;
            while (*value != '\0' && space((unsigned char)*value)) value++;
            if (*value != '\0') {
                if (r->nodes[index].text[0] != '\0') { (void)fail(r, "split text is unsupported"); return 0U; }
                r->nodes[index].text = value;
            }
        }
    }
    (void)fail(r, "unterminated element"); return 0U;
}

static size_t child(const XMLReader *r, size_t parent, const char *key)
{
    size_t i;
    if (parent == 0U) return 0U;
    for (i = r->nodes[parent].first; i != 0U; i = r->nodes[i].next)
        if (!r->nodes[i].attribute && strcmp(r->nodes[i].name, key) == 0) return i;
    return 0U;
}

static const char *attribute(const XMLReader *r, size_t parent, const char *key)
{
    size_t i;
    if (parent == 0U) return "";
    for (i = r->nodes[parent].first; i != 0U; i = r->nodes[i].next)
        if (r->nodes[i].attribute && strcmp(r->nodes[i].name, key) == 0) return r->nodes[i].text;
    return "";
}

static const char *value(const XMLReader *r, size_t parent, const char *key, const char *fallback)
{
    size_t i = child(r, parent, key);
    return i == 0U ? fallback : r->nodes[i].text;
}

/* MusicXML decimal numbers are locale independent and do not use exponents. */
static int decimal(const char *s, double *out)
{
    double number = 0.0, scale = 0.1;
    int negative = 0, digits = 0;
    while (space((unsigned char)*s)) s++;
    if (*s == '-' || *s == '+') { negative = *s == '-'; s++; }
    while (*s >= '0' && *s <= '9') { number = number*10.0+(double)(*s++-'0'); digits = 1; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { number += (double)(*s++-'0')*scale; scale *= 0.1; digits = 1; }
    }
    while (space((unsigned char)*s)) s++;
    if (*s != '\0' || !digits || !isfinite(number) || number > 1e12) return -1;
    *out = negative ? -number : number;
    return 0;
}

static int number(XMLReader *r, size_t parent, const char *key, double *out)
{
    size_t n = child(r, parent, key), i;
    if (n == 0U) return fail(r, "missing numeric element");
    for (i = r->nodes[n].next; i != 0U; i = r->nodes[i].next)
        if (!r->nodes[i].attribute && strcmp(r->nodes[i].name, key) == 0)
            return fail(r, "duplicate numeric element");
    for (i = r->nodes[n].first; i != 0U; i = r->nodes[i].next)
        if (!r->nodes[i].attribute) return fail(r, "nested numeric content");
    return decimal(r->nodes[n].text, out) == 0 ? 0 : fail(r, "invalid numeric element");
}

static int append(XMLReader *r, const HWAMusicXMLEvent *event, size_t measure)
{
    size_t count = r->score->event_count;
    if (count == r->limits.max_events) return fail(r, "event limit exceeded");
    if (count == r->event_capacity) {
        size_t cap = count == 0U ? 64U : count*2U;
        HWAMusicXMLEvent *events;
        size_t *indices;
        if (cap < count || cap > r->limits.max_events) cap = r->limits.max_events;
        events = grow(r, r->score->events, r->event_capacity, cap, sizeof(*events));
        if (events == NULL) return -1;
        r->score->events = events;
        indices = grow(r, r->event_measures, r->event_capacity, cap, sizeof(*indices));
        if (indices == NULL) return -1;
        r->event_measures = indices; r->event_capacity = cap;
    }
    r->score->events[count] = *event;
    r->event_measures[count] = measure;
    r->score->event_count++;
    return 0;
}

static int measure_slot(XMLReader *r, size_t index)
{
    if (index == r->measure_count) {
        if (index == r->measure_capacity) {
            size_t cap = index == 0U ? 32U : index*2U;
            XMLMeasure *p;
            if (cap < index || cap > r->limits.max_nodes) cap = r->limits.max_nodes;
            if (cap <= index) return fail(r, "measure limit exceeded");
            p = grow(r, r->measures, r->measure_capacity, cap, sizeof(*p));
            if (p == NULL) return -1;
            r->measures = p; r->measure_capacity = cap;
        }
        memset(&r->measures[index], 0, sizeof(r->measures[index]));
        r->measures[index].head = r->measures[index].tail = SIZE_MAX;
        r->measures[index].ending_end = SIZE_MAX;
        r->measures[index].jump_at = -1.0;
        r->measure_count++;
    }
    return 0;
}

static double beat_unit(const char *s)
{
    static const char *const names[] = {"long", "breve", "whole", "half", "quarter", "eighth",
        "16th", "32nd", "64th", "128th", "256th", "512th", "1024th"};
    double duration = 16.0;
    size_t i;
    for (i = 0U; i < sizeof(names)/sizeof(names[0]); i++, duration *= 0.5)
        if (strcmp(s, names[i]) == 0) return duration;
    return 0.0;
}

static int unique_fields(XMLReader *r, size_t parent, const char *const *fields, size_t count)
{
    size_t f;
    for (f = 0U; f < count; f++) {
        size_t first = child(r, parent, fields[f]), next;
        if (first == 0U) continue;
        for (next = r->nodes[first].next; next != 0U; next = r->nodes[next].next)
            if (!r->nodes[next].attribute && strcmp(r->nodes[next].name, fields[f]) == 0)
                return fail(r, "duplicate score field");
    }
    return 0;
}

static int repair_tuplet(XMLReader *r, size_t note, double divisions, double ticks,
                         HWAMusicXMLEvent *event)
{
    static const char *const fields[] = {"actual-notes", "normal-notes", "normal-type"};
    size_t modification = child(r, note, "time-modification"), n;
    double actual, normal, written, dot, expected;
    if (!r->limits.repair_tuplets || modification == 0U) return 0;
    if (unique_fields(r, modification, fields, sizeof(fields)/sizeof(fields[0])) != 0 ||
        number(r, modification, "actual-notes", &actual) != 0 ||
        number(r, modification, "normal-notes", &normal) != 0) return -1;
    if (actual < 1.0 || normal < 1.0 || floor(actual) != actual || floor(normal) != normal)
        return fail(r, "invalid tuplet ratio");
    written = beat_unit(value(r, note, "type", ""));
    if (written == 0.0 || floor(divisions) != divisions || floor(ticks) != ticks) return 0;
    dot = written*0.5;
    for (n = r->nodes[note].first; n != 0U; n = r->nodes[n].next) {
        if (!r->nodes[n].attribute && strcmp(r->nodes[n].name, "dot") == 0) {
            written += dot; dot *= 0.5;
        }
    }
    written *= normal/actual;
    expected = written*divisions;
    /* Only infer rounding when the duration is the nearest integer tick.
     * Fractional source durations and larger intentional differences win. */
    if (!isfinite(expected) || expected <= 0.0 || floor(expected+0.5) != ticks ||
        fabs(expected-ticks) <= 1e-9) return 0;
    event->duration_beats = written;
    event->interpretation |= 4U;
    r->score->repaired_tuplets++;
    return 0;
}

static int meter(XMLReader *r, size_t time, double *length)
{
    size_t i;
    double pending = 0.0, total = 0.0;
    if (child(r, time, "senza-misura") != 0U) { *length = 0.0; return 0; }
    for (i = r->nodes[time].first; i != 0U; i = r->nodes[i].next) {
        const char *key = r->nodes[i].name;
        if (r->nodes[i].attribute) continue;
        if (strcmp(key, "beats") == 0) {
            const char *s = r->nodes[i].text;
            if (pending != 0.0) return fail(r, "unpaired meter beats");
            do {
                double n = 0.0;
                int digits = 0;
                while (space((unsigned char)*s)) s++;
                while (*s >= '0' && *s <= '9') { digits = 1; n = n*10.0+(double)(*s++-'0'); }
                if (!digits || n < 1.0 || n > 1024.0) return fail(r, "invalid meter beats");
                pending += n;
                while (space((unsigned char)*s)) s++;
                if (*s == '\0') break;
                if (*s++ != '+') return fail(r, "unsupported meter");
            } while (1);
        } else if (strcmp(key, "beat-type") == 0) {
            double denominator;
            if (pending == 0.0 || decimal(r->nodes[i].text, &denominator) != 0 ||
                denominator < 1.0 || denominator > 1024.0 || floor(denominator) != denominator)
                return fail(r, "invalid meter denominator");
            total += pending*4.0/denominator; pending = 0.0;
        }
    }
    if (pending != 0.0 || total <= 0.0) return fail(r, "incomplete meter");
    *length = total; return 0;
}

static int tempo(XMLReader *r, size_t n, double *bpm)
{
    size_t sound = strcmp(r->nodes[n].name, "sound") == 0 ? n : child(r, n, "sound");
    const char *s = attribute(r, sound, "tempo");
    size_t type;
    *bpm = 0.0;
    if (*s != '\0') {
        if (decimal(s, bpm) != 0 || *bpm <= 0.0) return fail(r, "invalid sound tempo");
        return 0;
    }
    for (type = r->nodes[n].first; type != 0U; type = r->nodes[type].next) {
        size_t metronome = child(r, type, "metronome"), i;
        double unit, multiplier = 1.0, addition = 0.5, rate;
        if (metronome == 0U || r->nodes[type].attribute) continue;
        unit = beat_unit(value(r, metronome, "beat-unit", ""));
        if (unit == 0.0 || number(r, metronome, "per-minute", &rate) != 0 || rate <= 0.0)
            return fail(r, "unsupported metronome; supply sound tempo in quarter notes/minute");
        for (i = r->nodes[metronome].first; i != 0U; i = r->nodes[i].next) {
            if (!r->nodes[i].attribute && strcmp(r->nodes[i].name, "beat-unit-dot") == 0) {
                multiplier += addition; addition *= 0.5;
            }
        }
        if (*bpm != 0.0) return fail(r, "multiple metronomes in one direction");
        *bpm = rate*unit*multiplier;
    }
    return 0;
}

static int unsupported(XMLReader *r)
{
    static const char *const forbidden[] = {
        "measure-repeat", "beat-repeat", "multiple-rest",
        "swing", "unpitched", "cue", "part-link", "concert-score", "for-part"
    };
    size_t i, j;
    for (i = 1U; i <= r->count; i++) {
        if (r->nodes[i].attribute) continue;
        r->pos = r->nodes[i].start;
        if (strchr(r->nodes[i].name, ':') != NULL || *attribute(r, i, "xmlns") != '\0')
            return fail(r, "namespaced MusicXML elements are unsupported");
        for (j = 0U; j < sizeof(forbidden)/sizeof(forbidden[0]); j++) {
            if (strcmp(r->nodes[i].name, forbidden[j]) == 0) {
                hwa_set_error(r->error, r->error_size,
                    "MusicXML at byte %zu: unsupported <%s>; provide an unfolded pitched score",
                    r->pos, forbidden[j]); return -1;
            }
        }
        if (strcmp(r->nodes[i].name, "sound") == 0 && *attribute(r, i, "time-only") != '\0')
            return fail(r, "sound time-only traversal is unsupported");
        if (strcmp(r->nodes[i].name, "repeat") == 0 && strcmp(attribute(r, i, "after-jump"), "yes") == 0)
            return fail(r, "after-jump repeats need an unfolded score");
        if (strcmp(r->nodes[i].name, "note") == 0 &&
            (*attribute(r, i, "attack") != '\0' || *attribute(r, i, "release") != '\0' ||
             *attribute(r, i, "time-only") != '\0' || child(r, i, "play") != 0U))
            return fail(r, "note playback overrides are unsupported");
    }
    return 0;
}

static int numeric_attribute(XMLReader *r, size_t n, const char *key, double fallback, double *out)
{
    const char *s = attribute(r, n, key);
    *out = fallback;
    return *s == '\0' || decimal(s, out) == 0 ? 0 : fail(r, "invalid numeric playback attribute");
}

static int mark_event(XMLReader *r, const HWAMusicXMLEvent *parent, size_t n, size_t measure)
{
    HWAMusicXMLEvent event = *parent;
    event.kind = HWA_MUSICXML_MARK; event.duration_beats = 0.0;
    event.written_duration_beats = 0.0; event.velocity_valid = 0;
    event.mark = r->nodes[n].name;
    if (strcmp(event.mark, "words") == 0) event.mark = r->nodes[n].text;
    event.source_offset = r->nodes[n].start; event.source_size = r->nodes[n].end-r->nodes[n].start;
    return append(r, &event, measure);
}

static int controls(XMLReader *r, const HWAMusicXMLEvent *parent, size_t n, size_t measure)
{
    static const char *const pedals[] = {"damper-pedal", "sostenuto-pedal", "soft-pedal"};
    static const unsigned cc[] = {64U, 66U, 67U};
    size_t sound = strcmp(r->nodes[n].name, "sound") == 0 ? n : child(r, n, "sound"), i;
    HWAMusicXMLEvent event = *parent;
    double offset;
    event.voice = value(r, n, "voice", "");
    if (sound != 0U && child(r, sound, "offset") != 0U) {
        /* Parent start already uses the effective sound offset. */
        event.start_beats = parent->start_beats;
    }
    for (i = 0U; i < 3U; i++) {
        const char *s = attribute(r, sound, pedals[i]);
        if (*s == '\0') continue;
        if (strcmp(s, "yes") == 0) offset = 100.0;
        else if (strcmp(s, "no") == 0) offset = 0.0;
        else if (decimal(s, &offset) != 0) return fail(r, "invalid pedal value");
        if (offset < 0.0 || offset > 100.0) return fail(r, "pedal percentage outside 0..100");
        event.kind = HWA_MUSICXML_CONTROL; event.controller = cc[i]; event.value = offset*1.27;
        event.mark = pedals[i]; event.interpretation = 2U;
        if (append(r, &event, measure) != 0) return -1;
    }
    if (*attribute(r, sound, "dynamics") != '\0') {
        if (numeric_attribute(r, sound, "dynamics", 0.0, &offset) != 0 || offset < 0.0) return fail(r, "invalid dynamics");
        event.kind = HWA_MUSICXML_MARK; event.controller = 0U; event.value = fmin(127.0, offset*0.9);
        event.mark = "velocity"; event.interpretation = 2U;
        if (append(r, &event, measure) != 0) return -1;
    }
    for (i = r->nodes[n].first; i != 0U; i = r->nodes[i].next) {
        size_t j;
        if (r->nodes[i].attribute || strcmp(r->nodes[i].name, "direction-type") != 0) continue;
        for (j = r->nodes[i].first; j != 0U; j = r->nodes[j].next) {
            const char *tag = r->nodes[j].name;
            if (r->nodes[j].attribute || strcmp(tag, "metronome") == 0) continue;
            if (strcmp(tag, "dynamics") == 0) {
                size_t d;
                for (d = r->nodes[j].first; d != 0U; d = r->nodes[d].next)
                    if (!r->nodes[d].attribute && mark_event(r, parent, d, measure) != 0) return -1;
            } else if (strcmp(tag, "pedal") == 0) {
                const char *type = attribute(r, j, "type");
                double number;
                unsigned pedal;
                if (numeric_attribute(r, j, "number", 1.0, &number) != 0 ||
                    number < 1.0 || number > 16.0 || floor(number) != number) return fail(r, "invalid pedal number");
                pedal = (unsigned)number;
                event = *parent; event.kind = HWA_MUSICXML_CONTROL; event.mark = "damper-pedal";
                if (strcmp(type, "sostenuto") == 0) r->pedal_cc[pedal] = 66U;
                else if (strcmp(type, "start") == 0) r->pedal_cc[pedal] = 64U;
                event.controller = r->pedal_cc[pedal] ? r->pedal_cc[pedal] : 64U; event.interpretation = 1U;
                if (event.controller == 66U) event.mark = "sostenuto-pedal";
                if (strcmp(type, "stop") == 0) r->pedal_cc[pedal] = 0U;
                if (strcmp(type, "continue") == 0 || strcmp(type, "resume") == 0 || strcmp(type, "discontinue") == 0) {
                    if (mark_event(r, parent, j, measure) != 0) return -1;
                    continue;
                }
                if (strcmp(type, "start") != 0 && strcmp(type, "sostenuto") != 0 && strcmp(type, "stop") != 0 && strcmp(type, "change") != 0)
                    return fail(r, "invalid pedal mark type");
                /* Explicit playback data wins over its displayed pedal mark. */
                if (*attribute(r, sound, event.controller == 66U ? "sostenuto-pedal" : "damper-pedal") != '\0') continue;
                event.value = strcmp(type, "stop") == 0 || strcmp(type, "change") == 0 ? 0.0 : 127.0;
                if (append(r, &event, measure) != 0) return -1;
                if (strcmp(type, "change") == 0) {
                    event.value = 127.0; event.sequence = 1U;
                    if (append(r, &event, measure) != 0) return -1;
                }
            } else {
                if (mark_event(r, parent, j, measure) != 0) return -1;
                if (strcmp(tag, "segno") != 0 && strcmp(tag, "coda") != 0) r->score->unrendered_marks++;
            }
        }
    }
    return 0;
}

static int note_expression(XMLReader *r, HWAMusicXMLEvent *event, size_t n, size_t measure, double divisions)
{
    size_t notation, ornament = 0U, grace = child(r, n, "grace");
    double gate = 1.0;
    event->written_duration_beats = event->duration_beats;
    event->grace_previous = event->grace_following = event->grace_make = -1.0;
    if (*attribute(r, n, "dynamics") != '\0') {
        double v;
        if (numeric_attribute(r, n, "dynamics", 0.0, &v) != 0 || v < 0.0) return fail(r, "invalid note dynamics");
        event->velocity = fmin(127.0, v*0.9); event->velocity_valid = 1; event->interpretation |= 2U;
    }
    if (grace != 0U) {
        if (numeric_attribute(r, grace, "steal-time-previous", -1.0, &event->grace_previous) != 0 ||
            numeric_attribute(r, grace, "steal-time-following", -1.0, &event->grace_following) != 0 ||
            numeric_attribute(r, grace, "make-time", -1.0, &event->grace_make) != 0) return -1;
        if (event->grace_previous < -1.0 || event->grace_previous > 100.0 ||
            event->grace_following < -1.0 || event->grace_following > 100.0 || event->grace_make < -1.0)
            return fail(r, "invalid grace timing");
        if (event->grace_make >= 0.0) {
            if (divisions <= 0.0) return fail(r, "grace make-time needs divisions");
            event->grace_make /= divisions;
        }
    }
    for (notation = r->nodes[n].first; notation != 0U; notation = r->nodes[notation].next) {
        size_t group;
        if (r->nodes[notation].attribute || strcmp(r->nodes[notation].name, "notations") != 0) continue;
        for (group = r->nodes[notation].first; group != 0U; group = r->nodes[group].next) {
            size_t mark;
            const char *group_name = r->nodes[group].name;
            if (r->nodes[group].attribute) continue;
            if (strcmp(group_name, "articulations") != 0 && strcmp(group_name, "ornaments") != 0) {
                if (strcmp(group_name, "tied") != 0 && mark_event(r, event, group, measure) != 0) return -1;
                continue;
            }
            for (mark = r->nodes[group].first; mark != 0U; mark = r->nodes[mark].next) {
                const char *s = r->nodes[mark].name;
                if (r->nodes[mark].attribute) continue;
                if (mark_event(r, event, mark, measure) != 0) return -1;
                if (strcmp(s, "staccato") == 0) event->articulations |= 1U;
                else if (strcmp(s, "staccatissimo") == 0) event->articulations |= 2U;
                else if (strcmp(s, "tenuto") == 0) event->articulations |= 4U;
                else if (strcmp(s, "accent") == 0) event->articulations |= 8U;
                else if (strcmp(s, "strong-accent") == 0) event->articulations |= 16U;
                else if (strcmp(s, "trill-mark") == 0 || strcmp(s, "mordent") == 0 || strcmp(s, "inverted-mordent") == 0 ||
                         strcmp(s, "turn") == 0 || strcmp(s, "inverted-turn") == 0 || strcmp(s, "delayed-turn") == 0 ||
                         strcmp(s, "delayed-inverted-turn") == 0) {
                    if (ornament != 0U) return fail(r, "multiple simultaneous ornaments need an explicit performance");
                    ornament = mark;
                } else r->score->unrendered_marks++;
            }
        }
    }
    if (!r->limits.performance || event->kind != HWA_MUSICXML_NOTE) return append(r, event, measure);
    if (event->articulations & 2U) gate = 0.25;
    else if (event->articulations & 1U) gate = r->limits.staccato_ratio;
    if (gate != 1.0) event->interpretation |= 1U;
    if (ornament != 0U) {
        const char *s = r->nodes[ornament].name, *step = attribute(r, ornament, "trill-step"), *start = attribute(r, ornament, "start-note");
        double count = strcmp(s, "trill-mark") == 0 ? fmax(3.0, ceil(event->duration_beats*r->limits.trill_notes_per_beat)) :
                       strstr(s, "turn") != NULL ? 4.0 : 3.0;
        double interval = 2.0, fraction = strstr(s, "delayed") ? 0.5 : 0.0;
        unsigned k, total;
        if (*step != '\0') {
            if (strcmp(step, "half") == 0) interval = 1.0;
            else if (strcmp(step, "unison") == 0) interval = 0.0;
            else if (strcmp(step, "whole") != 0) return fail(r, "invalid ornament step");
        }
        if (numeric_attribute(r, ornament, "beats", count, &count) != 0 || count < 2.0 || count > 1024.0 || floor(count) != count)
            return fail(r, "ornament note count must be 2..1024");
        if (strcmp(attribute(r, ornament, "accelerate"), "yes") == 0 || *attribute(r, ornament, "second-beat") != '\0' ||
            *attribute(r, ornament, "last-beat") != '\0' ||
            (*attribute(r, ornament, "two-note-turn") != '\0' && strcmp(attribute(r, ornament, "two-note-turn"), "none") != 0))
            r->score->unrendered_marks++;
        total = (unsigned)count;
        if (fraction > 0.0) {
            HWAMusicXMLEvent held = *event;
            held.duration_beats *= fraction; held.interpretation |= 1U;
            if (append(r, &held, measure) != 0) return -1;
        }
        for (k = 0U; k < total; k++) {
            HWAMusicXMLEvent played = *event;
            double offset = 0.0;
            if (strstr(s, "turn") != NULL) {
                static const int turn[] = {1,0,-1,0};
                offset = interval*(double)turn[k%4U]*(strstr(s, "inverted") ? -1.0 : 1.0);
            } else {
                unsigned alternate = k%2U;
                if (strcmp(start, "upper") == 0 || strcmp(start, "below") == 0) alternate = 1U-alternate;
                else if (*start != '\0' && strcmp(start, "main") != 0) return fail(r, "invalid ornament starting note");
                offset = (double)alternate*interval*(strcmp(s, "mordent") == 0 || strcmp(start, "below") == 0 ? -1.0 : 1.0);
            }
            played.start_beats += event->duration_beats*(fraction+(1.0-fraction)*(double)k/(double)total);
            played.duration_beats = event->duration_beats*(1.0-fraction)/(double)total*gate;
            played.midi_pitch += offset; played.interpretation |= 1U; played.sequence = k+1U;
            if (played.midi_pitch < 0.0 || played.midi_pitch > 127.0) return fail(r, "ornament pitch outside MIDI range");
            if (append(r, &played, measure) != 0) return -1;
        }
        return 0;
    }
    event->duration_beats *= gate;
    return append(r, event, measure);
}

static int ending_mask(XMLReader *r, const char *s, unsigned *mask)
{
    *mask = 0U;
    do {
        unsigned n = 0U;
        while (space((unsigned char)*s)) s++;
        if (*s < '0' || *s > '9') return fail(r, "invalid ending number");
        while (*s >= '0' && *s <= '9') {
            n = n*10U+(unsigned)(*s++-'0');
            if (n > 32U) return fail(r, "ending number exceeds 32");
        }
        if (n == 0U) return fail(r, "ending number must be positive");
        *mask |= 1U << (n-1U);
        while (space((unsigned char)*s)) s++;
        if (*s == '\0') return 0;
        if (*s++ != ',') return fail(r, "invalid ending number list");
    } while (1);
}

static int navigation(XMLReader *r, size_t n, size_t ordinal, double cursor)
{
    XMLMeasure *m = &r->measures[ordinal];
    if (strcmp(r->nodes[n].name, "barline") == 0) {
        size_t repeat = child(r, n, "repeat"), ending = child(r, n, "ending");
        const char *location = attribute(r, n, "location");
        if (repeat != 0U) {
            const char *direction = attribute(r, repeat, "direction"), *times = attribute(r, repeat, "times");
            double count = 2.0;
            if (strcmp(direction, "forward") == 0) {
                if (strcmp(location, "left") != 0) return fail(r, "forward repeat must be on a left barline");
                m->repeat_start = 1U;
            } else if (strcmp(direction, "backward") == 0) {
                if (*location != '\0' && strcmp(location, "right") != 0) return fail(r, "backward repeat must be on a right barline");
                if (*times != '\0' && decimal(times, &count) != 0) return fail(r, "invalid repeat count");
                if (count < 1.0 || count > 32.0 || floor(count) != count) return fail(r, "repeat count must be 1..32");
                if (m->repeat_times != 0U && m->repeat_times != (unsigned)count) return fail(r, "parts disagree on repeat count");
                m->repeat_times = (unsigned)count;
            } else return fail(r, "invalid repeat direction");
        }
        if (ending != 0U) {
            unsigned mask;
            const char *type = attribute(r, ending, "type");
            if (ending_mask(r, attribute(r, ending, "number"), &mask) != 0) return -1;
            if (strcmp(type, "start") == 0) {
                if (strcmp(location, "left") != 0) return fail(r, "ending must start on a left barline");
                if (m->ending_mask != 0U && m->ending_mask != mask) return fail(r, "parts disagree on ending");
                m->ending_mask = mask;
            } else if (strcmp(type, "stop") == 0 || strcmp(type, "discontinue") == 0) {
                if (*location != '\0' && strcmp(location, "right") != 0) return fail(r, "ending must stop on a right barline");
                if (m->ending_stop != 0U && m->ending_stop != mask) return fail(r, "parts disagree on ending stop");
                m->ending_stop = mask;
            } else return fail(r, "invalid ending type");
        }
    } else {
        static const char *const labels[] = {"segno", "coda", "dalsegno", "tocoda"};
        const char **destinations[] = {&m->segno, &m->coda, &m->dalsegno, &m->tocoda};
        size_t sound = strcmp(r->nodes[n].name, "sound") == 0 ? n : child(r, n, "sound"), j;
        if (sound == 0U) return 0;
        for (j = 0U; j < 4U; j++) {
            const char *label = attribute(r, sound, labels[j]);
            if (*label == '\0') continue;
            if (child(r, sound, "offset") != 0U || child(r, n, "offset") != 0U || (j < 2U && cursor != 0.0))
                return fail(r, "navigation signs must occur at measure boundaries without offsets");
            if (*destinations[j] != NULL && strcmp(*destinations[j], label) != 0) return fail(r, "parts disagree on navigation label");
            *destinations[j] = label;
            if (j >= 2U) m->jump_at = cursor;
        }
        if (*attribute(r, sound, "dacapo") != '\0') {
            if (strcmp(attribute(r, sound, "dacapo"), "yes") != 0) return fail(r, "invalid da capo");
            m->dacapo = 1;
            m->jump_at = cursor;
        }
        if (*attribute(r, sound, "fine") != '\0') {
            if (strcmp(attribute(r, sound, "fine"), "yes") != 0) return fail(r, "numeric fine duration is unsupported");
            m->fine = 1;
            m->jump_at = cursor;
        }
        if (strcmp(attribute(r, sound, "forward-repeat"), "yes") == 0) m->repeat_start = 1U;
    }
    return 0;
}

static int unfold(XMLReader *r)
{
    struct RepeatFrame { size_t begin; unsigned pass; } stack[64];
    size_t i, active_ending = SIZE_MAX, index = 0U, depth = 0U, steps = 0U, previous_index = SIZE_MAX;
    unsigned last_pass = 1U;
    int has_navigation = 0, jumped = 0, coda_taken = 0, result = -1;
    double clock = 0.0;
    HWAMusicXMLEvent *original;
    size_t *next, original_count, original_capacity;
    for (i = 0U; i < r->measure_count; i++) {
        XMLMeasure *m = &r->measures[i];
        if (m->repeat_start || m->repeat_times || m->ending_mask || m->dacapo || m->dalsegno || m->tocoda) has_navigation = 1;
        if (m->ending_mask) {
            if (active_ending != SIZE_MAX) return fail(r, "overlapping endings");
            active_ending = i;
        }
        if (m->ending_stop) {
            if (active_ending == SIZE_MAX || r->measures[active_ending].ending_mask != m->ending_stop)
                return fail(r, "unmatched ending stop");
            r->measures[active_ending].ending_end = i; active_ending = SIZE_MAX;
        }
    }
    if (active_ending != SIZE_MAX) return fail(r, "unclosed ending");
    for (i = 0U; i < r->measure_count; i++) if (r->measures[i].ending_mask) {
        size_t end = r->measures[i].ending_end, next_ending = end+1U;
        unsigned mask = r->measures[i].ending_mask, highest = 1U;
        if (!r->measures[end].repeat_times) continue;
        while (next_ending < r->measure_count && r->measures[next_ending].ending_mask) {
            mask |= r->measures[next_ending].ending_mask;
            next_ending = r->measures[next_ending].ending_end+1U;
        }
        while ((mask >>= 1U) != 0U) highest++;
        if (r->measures[end].repeat_times < highest) r->measures[end].repeat_times = highest;
    }
    if (!has_navigation) {
        if (r->measure_count > r->limits.max_measure_visits) return fail(r, "measure visit limit exceeded");
        r->score->measure_visits = r->measure_count;
        for (i = 0U; i < r->score->event_count; i++) {
            HWAMusicXMLEvent *e = &r->score->events[i];
            e->measure_index = r->event_measures[i]; e->occurrence = 1U;
        }
        return 0;
    }
    original = r->score->events; next = r->event_measures;
    original_count = r->score->event_count; original_capacity = r->event_capacity;
    for (i = 0U; i < original_count; i++) {
        XMLMeasure *m = &r->measures[next[i]];
        original[i].measure_index = next[i];
        if (m->tail != SIZE_MAX) next[m->tail] = i; else m->head = i;
        m->tail = i; next[i] = SIZE_MAX;
    }
    {
        double carry = r->limits.default_tempo_bpm;
        for (i = 0U; i < r->measure_count; i++) {
            XMLMeasure *m = &r->measures[i];
            size_t e;
            double last_at = -1.0, latest = carry;
            m->initial_tempo = carry;
            for (e = m->head; e != SIZE_MAX; e = next[e]) if (original[e].kind == HWA_MUSICXML_TEMPO) {
                if (original[e].start_beats == m->start) m->initial_tempo = original[e].tempo_bpm;
                if (original[e].start_beats >= last_at) { last_at = original[e].start_beats; latest = original[e].tempo_bpm; }
            }
            carry = latest;
        }
    }
    r->score->events = NULL; r->score->event_count = 0U; r->event_measures = NULL; r->event_capacity = 0U;
    while (index < r->measure_count) {
        XMLMeasure *m = &r->measures[index];
        unsigned pass;
        if (++steps > r->limits.max_measure_visits) { (void)fail(r, "measure visit limit exceeded"); goto done; }
        if (!jumped && m->repeat_start && (depth == 0U || stack[depth-1U].begin != index)) {
            if (depth == 64U) { (void)fail(r, "repeat nesting limit exceeded"); goto done; }
            stack[depth].begin = index; stack[depth++].pass = 1U;
        }
        pass = depth != 0U ? stack[depth-1U].pass : last_pass;
        if (m->ending_mask && !(m->ending_mask & (1U << (pass-1U)))) {
            size_t end = m->ending_end;
            if (end == SIZE_MAX) { (void)fail(r, "unclosed ending"); goto done; }
            if (r->measures[end].repeat_times && depth != 0U) { last_pass = stack[--depth].pass; }
            index = end+1U; continue;
        }
        m->visits++;
        r->score->measure_visits++;
        if (previous_index != SIZE_MAX && previous_index+1U != index && m->initial_tempo > 0.0) {
            HWAMusicXMLEvent restored;
            memset(&restored, 0, sizeof(restored));
            restored.kind = HWA_MUSICXML_TEMPO; restored.start_beats = clock;
            restored.written_start_beats = m->start; restored.measure_index = index; restored.occurrence = m->visits;
            restored.tempo_bpm = m->initial_tempo; restored.mark = "tempo-restore";
            restored.id = restored.part = restored.part_name = restored.voice = restored.staff = restored.measure = "";
            if (append(r, &restored, index) != 0) goto done;
        }
        previous_index = index;
        for (i = m->head; i != SIZE_MAX; i = next[i]) {
            HWAMusicXMLEvent event = original[i];
            event.start_beats = clock+(event.start_beats-m->start); event.occurrence = m->visits;
            if (append(r, &event, index) != 0) goto done;
        }
        clock += m->length;
        if (!isfinite(clock) || clock > 1e12) { (void)fail(r, "expanded score timing limit exceeded"); goto done; }
        if (jumped && m->fine) break;
        if (!jumped && (m->dacapo || m->dalsegno)) {
            size_t destination = m->dacapo ? 0U : SIZE_MAX;
            if (m->dacapo && m->dalsegno) { (void)fail(r, "conflicting playback jumps"); goto done; }
            if (m->dalsegno) for (i = 0U; i < r->measure_count; i++) {
                if (r->measures[i].segno && strcmp(r->measures[i].segno, m->dalsegno) == 0) {
                    if (destination != SIZE_MAX) { (void)fail(r, "duplicate segno label"); goto done; }
                    destination = i;
                }
            }
            if (destination == SIZE_MAX || destination > index) { (void)fail(r, "missing or forward segno destination"); goto done; }
            jumped = 1; last_pass = 2U; depth = 0U; index = destination; continue;
        }
        if (jumped && !coda_taken && m->tocoda) {
            size_t destination = SIZE_MAX;
            for (i = 0U; i < r->measure_count; i++) if (r->measures[i].coda && strcmp(r->measures[i].coda, m->tocoda) == 0) {
                if (destination != SIZE_MAX) { (void)fail(r, "duplicate coda label"); goto done; }
                destination = i;
            }
            if (destination == SIZE_MAX || destination <= index) { (void)fail(r, "missing or backward coda destination"); goto done; }
            coda_taken = 1; index = destination; continue;
        }
        if (!jumped && m->repeat_times) {
            if (depth == 0U) { stack[0].begin = 0U; stack[0].pass = last_pass; depth = 1U; }
            if (stack[depth-1U].pass < m->repeat_times) {
                stack[depth-1U].pass++; index = stack[depth-1U].begin; continue;
            }
            last_pass = stack[--depth].pass;
        } else if (depth == 0U && !m->ending_mask) last_pass = 1U;
        index++;
    }
    if (depth != 0U && !jumped) { (void)fail(r, "unclosed forward repeat"); goto done; }
    r->score->duration_beats = clock; r->score->unfolded = 1; result = 0;
done:
    free(original); free(next);
    r->work -= (uint64_t)original_capacity*(sizeof(*original)+sizeof(*next));
    return result;
}

static int read_part(XMLReader *r, size_t part, const char *part_name)
{
    size_t m, ordinal = 0U, expected_measures = r->measure_count;
    double divisions = 0.0, bar_length = 0.0, transpose = 0.0;
    const char *part_id = attribute(r, part, "id");
    memset(r->pedal_cc, 0, sizeof(r->pedal_cc));
    for (m = r->nodes[part].first; m != 0U; m = r->nodes[m].next) {
        size_t n;
        double cursor = 0.0, source_cursor = 0.0, extent = 0.0, last_start = 0.0, last_duration = 0.0;
        const char *last_voice = "";
        int last_note = 0, last_grace = 0;
        if (r->nodes[m].attribute) continue;
        r->pos = r->nodes[m].start;
        if (strcmp(r->nodes[m].name, "measure") != 0) return fail(r, "expected partwise measure");
        if (*attribute(r, m, "number") == '\0') return fail(r, "measure needs a number");
        if (r->score->part_count != 0U && ordinal >= expected_measures)
            return fail(r, "parts must have matching measure counts");
        if (measure_slot(r, ordinal) != 0) return -1;
        for (n = r->nodes[m].first; n != 0U; n = r->nodes[n].next) {
            const char *tag = r->nodes[n].name;
            HWAMusicXMLEvent event;
            size_t pitch;
            if (r->nodes[n].attribute) continue;
            r->pos = r->nodes[n].start;
            memset(&event, 0, sizeof(event));
            event.mark = "";
            event.grace_previous = event.grace_following = event.grace_make = -1.0;
            event.id = attribute(r, n, "id"); event.part = part_id; event.part_name = part_name;
            event.voice = value(r, n, "voice", "1"); event.staff = value(r, n, "staff", "1");
            event.measure = attribute(r, m, "number"); event.start_beats = cursor;
            event.source_offset = r->nodes[n].start; event.source_size = r->nodes[n].end-r->nodes[n].start;
            if (strcmp(tag, "attributes") == 0) {
                static const char *const fields[] = {"divisions", "time", "transpose"};
                size_t time = child(r, n, "time"), trans = child(r, n, "transpose");
                if (unique_fields(r, n, fields, sizeof(fields)/sizeof(fields[0])) != 0) return -1;
                if (child(r, n, "divisions") != 0U &&
                    (number(r, n, "divisions", &divisions) != 0 || divisions <= 0.0))
                    return fail(r, "divisions must be positive");
                if (time != 0U && (cursor != 0.0 || *attribute(r, time, "number") != '\0' ||
                    meter(r, time, &bar_length) != 0)) return fail(r, "unsupported or invalid meter change");
                if (trans != 0U) {
                    double chromatic, octave = 0.0;
                    if (*attribute(r, trans, "number") != '\0' || child(r, trans, "double") != 0U)
                        return fail(r, "staff-specific or doubled transposition is unsupported");
                    if (number(r, trans, "chromatic", &chromatic) != 0 ||
                        (child(r, trans, "octave-change") != 0U && number(r, trans, "octave-change", &octave) != 0)) return -1;
                    if (floor(octave) != octave || fabs(octave) > 10.0 || fabs(chromatic) > 127.0)
                        return fail(r, "invalid transposition");
                    transpose = chromatic+12.0*octave;
                }
            } else if (strcmp(tag, "backup") == 0 || strcmp(tag, "forward") == 0) {
                double duration;
                if (divisions <= 0.0 || number(r, n, "duration", &duration) != 0 || duration <= 0.0)
                    return fail(r, "invalid backup/forward duration or missing divisions");
                duration /= divisions;
                if (r->limits.repair_tuplets && strcmp(tag, "backup") == 0 && fabs(cursor-source_cursor) > 1e-9) {
                    /* Exporters rewind by either the bar length or the sum
                     * of rounded note durations. Both denote a full voice. */
                    if (fabs(duration-cursor) > 1e-9 && fabs(duration-source_cursor) > 1e-9)
                        return fail(r, "partial backup after repaired tuplets is unsupported");
                    cursor = source_cursor = 0.0;
                } else {
                    double shift = strcmp(tag, "backup") == 0 ? -duration : duration;
                    cursor += shift; source_cursor += shift;
                }
                if (!isfinite(cursor) || cursor < -1e-9) return fail(r, "backup crosses the start of a measure");
                if (cursor < 0.0) cursor = 0.0;
                if (cursor > extent) extent = cursor;
                last_note = 0;
            } else if (strcmp(tag, "note") == 0) {
                static const char *const fields[] = {"pitch", "rest", "grace", "chord", "voice", "staff", "duration", "type", "time-modification"};
                static const char *const pitch_fields[] = {"step", "alter", "octave"};
                size_t tie;
                double source_duration = 0.0;
                int grace = child(r, n, "grace") != 0U;
                int chord = child(r, n, "chord") != 0U;
                event.chord = chord;
                int rest = child(r, n, "rest") != 0U;
                if (unique_fields(r, n, fields, sizeof(fields)/sizeof(fields[0])) != 0) return -1;
                event.kind = grace ? HWA_MUSICXML_GRACE : rest ? HWA_MUSICXML_REST : HWA_MUSICXML_NOTE;
                if (!grace) {
                    double duration;
                    if (divisions <= 0.0 || number(r, n, "duration", &duration) != 0 || duration <= 0.0)
                        return fail(r, "invalid note duration or missing divisions");
                    event.duration_beats = duration/divisions;
                    source_duration = event.duration_beats;
                    if (repair_tuplet(r, n, divisions, duration, &event) != 0) return -1;
                } else if (child(r, n, "duration") != 0U) return fail(r, "grace notes must not contain duration");
                pitch = child(r, n, "pitch");
                if ((pitch != 0U) == rest || (rest && (grace || chord))) return fail(r, "note must contain one pitch or rest");
                if (pitch != 0U) {
                    static const int semitones[] = {9, 11, 0, 2, 4, 5, 7};
                    const char *step = value(r, pitch, "step", "");
                    double octave, alter = 0.0;
                    if (unique_fields(r, pitch, pitch_fields, sizeof(pitch_fields)/sizeof(pitch_fields[0])) != 0) return -1;
                    if (strlen(step) != 1U || step[0] < 'A' || step[0] > 'G' ||
                        number(r, pitch, "octave", &octave) != 0 || floor(octave) != octave || octave < 0.0 || octave > 9.0 ||
                        (child(r, pitch, "alter") != 0U && number(r, pitch, "alter", &alter) != 0))
                        return fail(r, "invalid pitch");
                    event.midi_pitch = 12.0*(octave+1.0)+(double)semitones[(unsigned)(step[0]-'A')]+alter+transpose;
                    if (event.midi_pitch < 0.0 || event.midi_pitch > 127.0) return fail(r, "sounding pitch is outside MIDI 0..127");
                }
                if (chord) {
                    if (!last_note || last_grace != grace || strcmp(last_voice, event.voice) != 0 ||
                        event.duration_beats > last_duration+1e-9) return fail(r, "chord has no compatible preceding note");
                    event.start_beats = last_start;
                } else {
                    last_start = cursor; last_duration = event.duration_beats;
                    last_voice = event.voice; last_grace = grace; last_note = !rest;
                    cursor += event.duration_beats;
                    source_cursor += source_duration;
                }
                for (tie = r->nodes[n].first; tie != 0U; tie = r->nodes[tie].next) {
                    if (!r->nodes[tie].attribute && strcmp(r->nodes[tie].name, "tie") == 0) {
                        const char *type = attribute(r, tie, "type");
                        unsigned bit = strcmp(type, "start") == 0 ? 1U : strcmp(type, "stop") == 0 ? 2U : 0U;
                        if (bit == 0U || rest || (event.tie & bit)) return fail(r, "invalid sound tie");
                        event.tie |= bit;
                    }
                }
                if (!isfinite(cursor) || !isfinite(event.duration_beats) ||
                    (!grace && event.duration_beats <= 0.0)) return fail(r, "note timing overflow");
                if (cursor > extent) extent = cursor;
                event.written_start_beats = event.start_beats;
                if (note_expression(r, &event, n, ordinal, divisions) != 0) return -1;
                if (grace) r->score->grace_count++;
            } else if (strcmp(tag, "direction") == 0 || strcmp(tag, "sound") == 0) {
                static const char *const fields[] = {"sound", "offset", "voice", "staff"};
                double offset = 0.0, bpm;
                size_t sound = strcmp(tag, "sound") == 0 ? n : child(r, n, "sound");
                event.voice = value(r, n, "voice", "");
                if (navigation(r, n, ordinal, cursor) != 0) return -1;
                if (unique_fields(r, n, fields, sizeof(fields)/sizeof(fields[0])) != 0) return -1;
                if (child(r, n, "offset") != 0U &&
                    (divisions <= 0.0 || number(r, n, "offset", &offset) != 0)) return fail(r, "invalid direction offset");
                if (offset != 0.0) offset /= divisions;
                event.start_beats += offset;
                if (event.start_beats < 0.0 || !isfinite(event.start_beats)) return fail(r, "direction offset crosses measure start");
                event.kind = HWA_MUSICXML_DIRECTION;
                event.written_start_beats = event.start_beats;
                if (append(r, &event, ordinal) != 0 || tempo(r, n, &bpm) != 0) return -1;
                if (sound != 0U && child(r, sound, "offset") != 0U) {
                    if (divisions <= 0.0 || number(r, sound, "offset", &offset) != 0) return fail(r, "invalid sound offset");
                    event.start_beats = cursor+offset/divisions;
                    if (!isfinite(event.start_beats) || event.start_beats < 0.0) return fail(r, "invalid sound position");
                    event.written_start_beats = event.start_beats;
                }
                if (controls(r, &event, n, ordinal) != 0) return -1;
                if (bpm != 0.0) {
                    if (sound != 0U && child(r, sound, "offset") != 0U) {
                        if (divisions <= 0.0 || number(r, sound, "offset", &offset) != 0) return fail(r, "invalid sound offset");
                        event.start_beats = cursor+offset/divisions;
                        if (event.start_beats < 0.0) return fail(r, "sound offset crosses measure start");
                    }
                    event.kind = HWA_MUSICXML_TEMPO; event.tempo_bpm = bpm;
                    if (append(r, &event, ordinal) != 0) return -1;
                }
            } else if (strcmp(tag, "barline") == 0) {
                if (navigation(r, n, ordinal, cursor) != 0) return -1;
            } else if (strcmp(tag, "print") != 0 &&
                       strcmp(tag, "harmony") != 0 && strcmp(tag, "figured-bass") != 0 &&
                       strcmp(tag, "bookmark") != 0 && strcmp(tag, "link") != 0 && strcmp(tag, "grouping") != 0)
                return fail(r, "unsupported measure content");
        }
        if (strcmp(attribute(r, m, "implicit"), "yes") != 0 && bar_length > 0.0) {
            if (extent > bar_length+1e-8*fmax(1.0, bar_length)) return fail(r, "measure exceeds its meter");
            extent = bar_length;
        }
        if (extent <= 0.0 || !isfinite(extent)) return fail(r, "measure has no positive time span");
        if (r->measures[ordinal].jump_at >= 0.0 && fabs(r->measures[ordinal].jump_at-extent) > 1e-8)
            return fail(r, "playback jump must follow the final measure duration");
        if (extent > r->measures[ordinal].length) r->measures[ordinal].length = extent;
        ordinal++;
    }
    if (ordinal == 0U || (r->score->part_count != 0U && ordinal != r->measure_count))
        return fail(r, "parts must have matching measure counts");
    r->score->part_count++;
    return 0;
}

static int event_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = left, *b = right;
    if (a->start_beats != b->start_beats) return a->start_beats < b->start_beats ? -1 : 1;
    if ((a->kind == HWA_MUSICXML_TEMPO) != (b->kind == HWA_MUSICXML_TEMPO))
        return a->kind == HWA_MUSICXML_TEMPO ? -1 : 1;
    if (a->source_offset != b->source_offset) return a->source_offset < b->source_offset ? -1 : 1;
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence ? 1 : 0;
}

static int read_score(XMLReader *r, size_t root)
{
    size_t part, list = child(r, root, "part-list"), i, output, last_tempo = SIZE_MAX, definitions = 0U;
    double cursor = 0.0;
    if (strcmp(r->nodes[root].name, "score-partwise") != 0 || list == 0U)
        return fail(r, "expected score-partwise with part-list; timewise scores are unsupported");
    if (unsupported(r) != 0) return -1;
    for (i = r->nodes[list].first; i != 0U; i = r->nodes[i].next)
        if (!r->nodes[i].attribute && strcmp(r->nodes[i].name, "score-part") == 0) definitions++;
    for (part = r->nodes[root].first; part != 0U; part = r->nodes[part].next) {
        const char *id, *part_name = NULL;
        size_t definition, previous;
        if (r->nodes[part].attribute || strcmp(r->nodes[part].name, "part") != 0) continue;
        if (r->score->part_count == 256U) return fail(r, "part limit exceeded");
        id = attribute(r, part, "id");
        if (*id == '\0') return fail(r, "part needs an id");
        for (previous = r->nodes[root].first; previous != part; previous = r->nodes[previous].next)
            if (!r->nodes[previous].attribute && strcmp(r->nodes[previous].name, "part") == 0 &&
                strcmp(attribute(r, previous, "id"), id) == 0) return fail(r, "duplicate part id");
        for (definition = r->nodes[list].first; definition != 0U; definition = r->nodes[definition].next) {
            if (!r->nodes[definition].attribute && strcmp(r->nodes[definition].name, "score-part") == 0 &&
                strcmp(attribute(r, definition, "id"), id) == 0) {
                if (part_name != NULL) return fail(r, "duplicate score-part definition");
                part_name = value(r, definition, "part-name", "");
            }
        }
        if (part_name == NULL) return fail(r, "part id is absent from part-list");
        if (read_part(r, part, part_name) != 0) return -1;
    }
    if (r->score->part_count == 0U || r->score->event_count == 0U) return fail(r, "score has no events");
    if (definitions != r->score->part_count) return fail(r, "part-list and score parts do not match");
    for (i = 0U; i < r->measure_count; i++) {
        r->measures[i].start = cursor; cursor += r->measures[i].length;
        if (!isfinite(cursor) || cursor > 1e12) return fail(r, "score timing limit exceeded");
    }
    r->score->duration_beats = cursor;
    for (i = 0U; i < r->score->event_count; i++) {
        HWAMusicXMLEvent *event = &r->score->events[i];
        if (event->start_beats > r->measures[r->event_measures[i]].length+1e-8)
            return fail(r, "event starts beyond its measure");
        event->start_beats += r->measures[r->event_measures[i]].start;
        event->written_start_beats += r->measures[r->event_measures[i]].start;
    }
    if (unfold(r) != 0) return -1;
    qsort(r->score->events, r->score->event_count, sizeof(*r->score->events), event_order);
    for (i = 0U, output = 0U; i < r->score->event_count; i++) {
        HWAMusicXMLEvent *event = &r->score->events[i];
        if (event->kind == HWA_MUSICXML_TEMPO) {
            if (last_tempo != SIZE_MAX && r->score->events[last_tempo].start_beats == event->start_beats) {
                if (r->score->events[last_tempo].tempo_bpm != event->tempo_bpm) {
                    if (!r->limits.last_tempo_wins) {
                        r->pos = event->source_offset;
                        return fail(r, "conflicting tempos at one beat");
                    }
                    r->score->events[last_tempo] = *event;
                    r->score->events[last_tempo].interpretation |= 8U;
                    r->score->tempo_conflicts++;
                }
                continue;
            }
            last_tempo = output;
        }
        r->score->events[output++] = *event;
    }
    r->score->event_count = output;
    if (r->limits.default_tempo_bpm > 0.0 &&
        (r->score->events[0].kind != HWA_MUSICXML_TEMPO || r->score->events[0].start_beats != 0.0)) {
        HWAMusicXMLEvent event;
        memset(&event, 0, sizeof(event));
        event.kind = HWA_MUSICXML_TEMPO; event.tempo_bpm = r->limits.default_tempo_bpm;
        event.id = event.part = event.part_name = event.voice = event.staff = event.measure = "";
        event.mark = "";
        if (append(r, &event, 0U) != 0) return -1;
        r->score->used_default_tempo = 1;
        qsort(r->score->events, r->score->event_count, sizeof(*r->score->events), event_order);
    }
    return 0;
}

static int read_document(const unsigned char *data, size_t size,
                      const HWAMusicXMLOptions *options,
                      HWAMusicXMLScore *score, const char **member, char *error, size_t error_size)
{
    XMLReader r;
    size_t root = 0U;
    int result = -1, doctype = 0;
    memset(&r, 0, sizeof(r));
    if (options != NULL) r.limits = *options;
    else hwa_musicxml_options_default(&r.limits);
    if (score != NULL) memset(score, 0, sizeof(*score));
    if (error != NULL && error_size != 0U) error[0] = '\0';
    r.data = data; r.size = size; r.score = score; r.error = error; r.error_size = error_size;
    if (data == NULL || score == NULL || size == 0U || (uint64_t)size > r.limits.max_input_bytes ||
        size > (SIZE_MAX-1U)/2U || r.limits.max_nodes == 0U || r.limits.max_nodes == SIZE_MAX ||
        r.limits.max_events == 0U || r.limits.max_work_bytes == 0U || r.limits.max_measure_visits == 0U ||
        (r.limits.performance != 0 && r.limits.performance != 1) ||
        (r.limits.repair_tuplets != 0 && r.limits.repair_tuplets != 1) ||
        (r.limits.last_tempo_wins != 0 && r.limits.last_tempo_wins != 1) ||
        !isfinite(r.limits.grace_fraction) || r.limits.grace_fraction <= 0.0 || r.limits.grace_fraction >= 1.0 ||
        !isfinite(r.limits.trill_notes_per_beat) || r.limits.trill_notes_per_beat < 1.0 || r.limits.trill_notes_per_beat > 128.0 ||
        !isfinite(r.limits.staccato_ratio) || r.limits.staccato_ratio <= 0.0 || r.limits.staccato_ratio > 1.0 ||
        !isfinite(r.limits.default_velocity) || r.limits.default_velocity < 0.0 || r.limits.default_velocity > 127.0 ||
        !isfinite(r.limits.default_tempo_bpm) || r.limits.default_tempo_bpm < 0.0 || r.limits.default_tempo_bpm > 1e6)
        return fail(&r, "invalid input, options, or input byte limit");
    if (utf8(data, size) != 0) return fail(&r, "expected valid UTF-8 and XML 1.0 characters");
    r.strings_size = size*2U+1U;
    r.strings = grow(&r, NULL, 0U, r.strings_size, 1U);
    if (r.strings == NULL) return -1;
    score->storage = r.strings;
    if (size >= 3U && memcmp(data, "\xef\xbb\xbf", 3U) == 0) r.pos = 3U;
    for (;;) {
        int misc;
        skip_space(&r);
        misc = skip_misc(&r);
        if (misc < 0) goto done;
        if (misc != 0) continue;
        if (at(&r, "<!DOCTYPE")) {
            unsigned char quote = 0U;
            if (doctype || root != 0U) { (void)fail(&r, "misplaced DOCTYPE"); goto done; }
            doctype = 1; r.pos += 9U;
            while (r.pos < size) {
                unsigned char c = data[r.pos++];
                if (quote != 0U) { if (c == quote) quote = 0U; }
                else if (c == '\'' || c == '"') quote = c;
                else if (c == '[') { (void)fail(&r, "internal DTDs/entities are unsupported"); goto done; }
                else if (c == '>') break;
            }
            if (r.pos == size) { (void)fail(&r, "unterminated DOCTYPE"); goto done; }
            continue;
        }
        if (root != 0U) {
            if (r.pos != size) { (void)fail(&r, "content after root element"); goto done; }
            break;
        }
        if (!at(&r, "<") || (root = element(&r, 0U, 1U)) == 0U) {
            if (error != NULL && error_size != 0U && error[0] == '\0') (void)fail(&r, "missing root element");
            goto done;
        }
    }
    if (member != NULL) {
        size_t files = child(&r, root, "rootfiles"), entry = child(&r, files, "rootfile");
        const char *media = attribute(&r, entry, "media-type");
        if (strcmp(r.nodes[root].name, "container") != 0 || entry == 0U ||
            *attribute(&r, entry, "full-path") == '\0' ||
            (*media != '\0' && strcmp(media, "application/vnd.recordare.musicxml+xml") != 0)) {
            (void)fail(&r, "invalid MusicXML container rootfile"); goto done;
        }
        *member = attribute(&r, entry, "full-path"); result = 0;
    } else {
        result = read_score(&r, root);
        if (result == 0) result = hwa_musicxml_perform(score, &r.limits,
            r.limits.max_work_bytes-r.work, error, error_size);
    }
done:
    free(r.nodes); free(r.measures); free(r.event_measures);
    if (result != 0) hwa_musicxml_score_free(score);
    return result;
}

int hwa_musicxml_read(const unsigned char *data, size_t size,
                     const HWAMusicXMLOptions *options,
                     HWAMusicXMLScore *score, char *error, size_t error_size)
{
    HWAMusicXMLOptions limits, parse_limits;
    unsigned char *xml = NULL;
    size_t xml_size = size;
    int result;
    if (options != NULL) limits = *options; else hwa_musicxml_options_default(&limits);
    if (score != NULL) memset(score, 0, sizeof(*score));
    if (data == NULL || score == NULL || size == 0U || size == SIZE_MAX ||
        (uint64_t)size > limits.max_input_bytes || (uint64_t)size+1U >= limits.max_work_bytes) {
        hwa_set_error(error, error_size, "MusicXML: invalid input, input byte limit or work byte limit"); return -1;
    }
    parse_limits = limits;
    if (size >= 2U && data[0] == 'P' && data[1] == 'K') {
        unsigned char *container = NULL;
        size_t container_size = 0U;
        HWAMusicXMLScore container_tree;
        const char *member = NULL;
        uint64_t cap = limits.max_input_bytes < limits.max_work_bytes ? limits.max_input_bytes : limits.max_work_bytes;
        if (hwa_musicxml_zip_member(data, size, "META-INF/container.xml", cap,
                &container, &container_size, error, error_size) != 0) return -1;
        parse_limits.max_work_bytes -= (uint64_t)container_size+1U;
        result = read_document(container, container_size, &parse_limits, &container_tree, &member, error, error_size);
        if (result == 0) {
            uint64_t occupied = (uint64_t)container_size*3U+2U;
            if (occupied >= limits.max_work_bytes) {
                hwa_set_error(error, error_size, "MusicXML container exceeds work byte limit"); result = -1;
            } else {
                cap = limits.max_work_bytes-occupied;
                if (cap > limits.max_input_bytes) cap = limits.max_input_bytes;
                result = hwa_musicxml_zip_member(data, size, member, cap, &xml, &xml_size, error, error_size);
            }
        }
        hwa_musicxml_score_free(&container_tree); free(container);
        if (result != 0) return -1;
    } else {
        xml = malloc(size+1U);
        if (xml == NULL) { hwa_set_error(error, error_size, "MusicXML: out of memory"); return -1; }
        memcpy(xml, data, size); xml[size] = 0U;
    }
    parse_limits = limits;
    parse_limits.max_work_bytes -= (uint64_t)xml_size+1U;
    result = read_document(xml, xml_size, &parse_limits, score, NULL, error, error_size);
    if (result != 0) free(xml);
    else { score->xml_data = xml; score->xml_size = xml_size; }
    return result;
}

void hwa_musicxml_options_default(HWAMusicXMLOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->max_events = 100000U; options->max_nodes = 1000000U;
    options->max_input_bytes = UINT64_C(32)*1024U*1024U;
    options->max_work_bytes = UINT64_C(128)*1024U*1024U;
    options->default_tempo_bpm = 120.0;
    options->max_measure_visits = 100000U;
    options->grace_fraction = 0.125; options->trill_notes_per_beat = 8.0;
    options->staccato_ratio = 0.5; options->default_velocity = 64.0;
}

void hwa_musicxml_score_free(HWAMusicXMLScore *score)
{
    if (score == NULL) return;
    free(score->events); free(score->storage); free(score->xml_data);
    memset(score, 0, sizeof(*score));
}
