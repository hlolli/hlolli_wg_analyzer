#include <hlolli_wg_analyzer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char prefix[] = "<?xml version='1.0' encoding='UTF-8'?>"
    "<!DOCTYPE score-partwise PUBLIC '-//Recordare//DTD MusicXML 4.0 Partwise//EN' 'https://invalid.example/no.dtd'>"
    "<score-partwise version='4.0'><part-list><score-part id='A'><part-name>A &amp; B &#x1D11E;</part-name>"
    "</score-part></part-list><part id='A'><measure number='1'>";
static const char suffix[] = "</measure></part></score-partwise>";
static const char note[] = "<note id='original'><pitch><step>C</step><octave>4</octave></pitch>"
    "<duration>1</duration><voice>one</voice><notations><ornaments><trill-mark/></ornaments></notations></note>";

static char *document(const char *body)
{
    size_t size = strlen(prefix)+strlen(body)+strlen(suffix)+1U;
    char *xml = malloc(size);
    CHECK(xml != NULL);
    if (xml != NULL) (void)snprintf(xml, size, "%s%s%s", prefix, body, suffix);
    return xml;
}

static int read(const char *xml, const HWAMusicXMLOptions *options, HWAMusicXMLScore *score)
{
    char error[HWA_ERROR_SIZE];
    int status = hwa_musicxml_read((const unsigned char *)xml, strlen(xml), options,
                                 score, error, sizeof(error));
    if (status != 0) fprintf(stderr, "unexpected import failure: %s\n", error);
    return status;
}

static void reject(const char *xml, const HWAMusicXMLOptions *options, const char *message)
{
    HWAMusicXMLScore score;
    char error[HWA_ERROR_SIZE];
    memset(&score, 0xa5, sizeof(score));
    CHECK(hwa_musicxml_read((const unsigned char *)xml, strlen(xml), options,
                            &score, error, sizeof(error)) != 0);
    CHECK(error[0] != '\0');
    if (message != NULL) {
        if (strstr(error, message) == NULL) fprintf(stderr, "expected %s, got %s\n", message, error);
        CHECK(strstr(error, message) != NULL);
    }
    CHECK(score.events == NULL && score.storage == NULL && score.event_count == 0U);
    hwa_musicxml_score_free(&score);
}

static void simple_and_limits(void)
{
    char body[2048], *xml;
    HWAMusicXMLScore score;
    HWAMusicXMLOptions options;
    size_t i;
    (void)snprintf(body, sizeof(body), "<attributes><divisions>3</divisions></attributes>%s", note);
    xml = document(body);
    if (xml == NULL) return;
    if (read(xml, NULL, &score) == 0) {
        const HWAMusicXMLEvent *n = &score.events[1];
        CHECK(score.event_count == 3U && score.part_count == 1U && score.used_default_tempo);
        CHECK(score.events[0].kind == HWA_MUSICXML_TEMPO && score.events[0].tempo_bpm == 120.0);
        CHECK(n->kind == HWA_MUSICXML_NOTE && n->midi_pitch == 60.0);
        CHECK(n->duration_beats == 1.0/3.0 && score.duration_beats == 1.0/3.0);
        CHECK(strcmp(n->id, "original") == 0 && strcmp(n->voice, "one") == 0);
        CHECK(strcmp(n->part_name, "A & B \xf0\x9d\x84\x9e") == 0);
        CHECK(n->source_size == strlen(note) && memcmp(xml+n->source_offset, note, n->source_size) == 0);
        free(xml); xml = NULL;
        CHECK(strcmp(n->part, "A") == 0); /* Strings do not alias input. */
        CHECK(score.xml_data != NULL && score.xml_size > n->source_offset+n->source_size);
        CHECK(memcmp(score.xml_data+n->source_offset, note, n->source_size) == 0);
        hwa_musicxml_score_free(&score);
    }
    free(xml); xml = document(body);
    if (xml == NULL) return;
    hwa_musicxml_options_default(&options);
    options.default_tempo_bpm = 0.0;
    if (read(xml, &options, &score) == 0) {
        CHECK(score.event_count == 2U && !score.used_default_tempo);
        hwa_musicxml_score_free(&score);
    }
    hwa_musicxml_options_default(&options); options.max_events = 1U;
    reject(xml, &options, "event limit");
    hwa_musicxml_options_default(&options); options.max_nodes = 1U;
    reject(xml, &options, "node limit");
    hwa_musicxml_options_default(&options); options.max_input_bytes = strlen(xml)-1U;
    reject(xml, &options, "input byte limit");
    hwa_musicxml_options_default(&options); options.max_work_bytes = 1U;
    reject(xml, &options, "work byte limit");
    hwa_musicxml_options_default(&options); options.default_tempo_bpm = NAN;
    reject(xml, &options, "options");
    hwa_musicxml_options_default(&options); options.repair_tuplets = 2;
    reject(xml, &options, "options");
    hwa_musicxml_options_default(&options); options.last_tempo_wins = -1;
    reject(xml, &options, "options");
    for (i = 0U; i < strlen(xml); i++) {
        char saved = xml[i]; xml[i] = '\0';
        reject(xml, NULL, NULL);
        xml[i] = saved;
    }
    free(xml);
}

static void timeline(void)
{
    static const char xml[] =
        "<score-partwise><part-list><score-part id='A'><part-name>Upper</part-name></score-part>"
        "<score-part id='B'><part-name>Lower</part-name></score-part></part-list>"
        "<part id='A'><measure number='0' implicit='yes'><attributes><divisions>6</divisions>"
        "<time><beats>4</beats><beat-type>4</beat-type></time></attributes>"
        "<direction><direction-type><metronome><beat-unit>quarter</beat-unit><beat-unit-dot/>"
        "<per-minute>80</per-minute></metronome></direction-type></direction>"
        "<note><grace/><pitch><step>D</step><octave>4</octave></pitch></note>"
        "<note><pitch><step>C</step><octave>4</octave></pitch><duration>3</duration><voice>1</voice><staff>1</staff></note>"
        "<note><chord/><pitch><step>E</step><octave>4</octave></pitch><duration>3</duration><voice>1</voice><staff>2</staff></note>"
        "<backup><duration>3</duration></backup>"
        "<note><pitch><step>G</step><octave>3</octave></pitch><duration>3</duration><voice>2</voice></note>"
        "</measure><measure number='1'><attributes><divisions>3</divisions></attributes>"
        "<direction><offset>3</offset><sound tempo='90'/></direction>"
        "<note><pitch><step>C</step><octave>4</octave></pitch><duration>3</duration><tie type='start'/></note>"
        "<note><pitch><step>C</step><octave>4</octave></pitch><duration>3</duration><tie type='stop'/></note>"
        "<forward><duration>6</duration></forward></measure></part>"
        "<part id='B'><measure number='0' implicit='yes'><attributes><divisions>2</divisions>"
        "<time><beats>4</beats><beat-type>4</beat-type></time>"
        "<transpose><diatonic>-1</diatonic><chromatic>-2</chromatic></transpose></attributes>"
        "<sound tempo='120'/><note><rest/><duration>1</duration></note></measure>"
        "<measure number='1'><note><pitch><step>D</step><alter>0.5</alter><octave>4</octave></pitch>"
        "<duration>8</duration></note></measure></part></score-partwise>";
    HWAMusicXMLScore score;
    size_t i, notes = 0U, tempos = 0U, grace = 0U;
    int lower = 0, tie_start = 0, tie_stop = 0, chord = 0;
    if (read(xml, NULL, &score) != 0) { CHECK(0); return; }
    CHECK(score.part_count == 2U && score.grace_count == 1U && !score.used_default_tempo);
    CHECK(score.duration_beats == 4.5);
    for (i = 0U; i < score.event_count; i++) {
        const HWAMusicXMLEvent *e = &score.events[i];
        if (i != 0U) CHECK(e->start_beats >= score.events[i-1U].start_beats);
        if (e->kind == HWA_MUSICXML_TEMPO) {
            CHECK((e->start_beats == 0.0 && e->tempo_bpm == 120.0) ||
                  (e->start_beats == 1.5 && e->tempo_bpm == 90.0));
            tempos++;
        } else if (e->kind == HWA_MUSICXML_GRACE) {
            CHECK(e->duration_beats == 0.0 && e->midi_pitch == 62.0); grace++;
        } else if (e->kind == HWA_MUSICXML_NOTE) {
            notes++;
            if (strcmp(e->part, "B") == 0) { CHECK(e->midi_pitch == 60.5 && e->start_beats == 0.5); lower = 1; }
            if (strcmp(e->staff, "2") == 0) { CHECK(e->start_beats == 0.0 && e->duration_beats == 0.5); chord = 1; }
            if (e->tie == 1U) { CHECK(e->start_beats == 0.5); tie_start = 1; }
            if (e->tie == 2U) { CHECK(e->start_beats == 1.5); tie_stop = 1; }
        }
    }
    CHECK(notes == 6U && tempos == 2U && grace == 1U && lower && chord && tie_start && tie_stop);
    hwa_musicxml_score_free(&score);
}

static void rounded_tuplets(void)
{
    char body[8192], *xml;
    HWAMusicXMLOptions options;
    HWAMusicXMLScore score;
    size_t used, i, pass;
    hwa_musicxml_options_default(&options);
    options.default_tempo_bpm = 0.0;
    options.repair_tuplets = 1;
    for (pass = 0U; pass < 3U; pass++) {
        /* Full-voice backups may use the bar length or the rounded sum. */
        unsigned ticks = 69U, divisions = pass == 2U ? 484U : 480U;
        unsigned backup = pass == 0U ? divisions : ticks*7U;
        used = (size_t)snprintf(body, sizeof(body),
            "<attributes><divisions>%u</divisions><time><beats>1</beats><beat-type>4</beat-type></time></attributes>", divisions);
        for (i = 0U; i < 7U; i++) {
            used += (size_t)snprintf(body+used, sizeof(body)-used,
                "<note id='t%zu'><pitch><step>C</step><octave>4</octave></pitch>"
                "<duration>%u</duration><voice>1</voice><type>16th</type>"
                "<time-modification><actual-notes>7</actual-notes><normal-notes>4</normal-notes>"
                "</time-modification></note>", i, ticks);
        }
        (void)snprintf(body+used, sizeof(body)-used,
            "<backup><duration>%u</duration></backup>"
            "<note id='lower'><pitch><step>C</step><octave>3</octave></pitch><duration>%u</duration><voice>2</voice></note>"
            "</measure><measure number='2'><note id='next'><rest/><duration>%u</duration></note>", backup, divisions, divisions);
        xml = document(body);
        if (xml == NULL) return;
        if (pass == 0U) reject(xml, NULL, "measure exceeds its meter");
        if (read(xml, &options, &score) == 0) {
            CHECK(score.repaired_tuplets == 7U && score.event_count == 9U);
            CHECK(fabs(score.duration_beats-2.0) < 1e-12);
            CHECK(score.xml_size == strlen(xml) && memcmp(score.xml_data, xml, strlen(xml)) == 0);
            for (i = 0U; i < score.event_count; i++) {
                const HWAMusicXMLEvent *e = &score.events[i];
                if (e->id[0] == 't') {
                    CHECK(fabs(e->duration_beats-1.0/7.0) < 1e-12);
                    CHECK(fabs(e->written_duration_beats-1.0/7.0) < 1e-12);
                    CHECK(fabs(e->start_beats-(double)(e->id[1]-'0')/7.0) < 1e-12);
                    CHECK((e->interpretation & 4U) != 0U);
                } else if (strcmp(e->id, "lower") == 0) CHECK(e->start_beats == 0.0);
                else if (strcmp(e->id, "next") == 0) CHECK(e->start_beats == 1.0);
            }
            hwa_musicxml_score_free(&score);
        } else CHECK(0);
        free(xml);
    }
}

static void malformed(void)
{
    static const char *const bodies[] = {
        "<attributes><divisions>0</divisions></attributes>",
        "<attributes><divisions>-2</divisions></attributes>",
        "<attributes><divisions>1</divisions><divisions>2</divisions></attributes>",
        "<attributes><divisions>1</divisions></attributes><note><rest/><rest/><duration>1</duration></note>",
        "<attributes><divisions>1</divisions></attributes><backup><duration>1</duration></backup>",
        "<note><rest/><duration>1</duration></note>",
        "<attributes><divisions>1</divisions></attributes><note><chord/><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration></note>",
        "<attributes><divisions>1</divisions></attributes><note><rest/><duration>-1</duration></note>",
        "<attributes><divisions>1</divisions></attributes><note><pitch><step>H</step><octave>4</octave></pitch><duration>1</duration></note>",
        "<attributes><divisions>1</divisions></attributes><note><rest/><duration>&unknown;</duration></note>",
        "<attributes><divisions>1</divisions></attributes><note><rest/><duration>&#0;</duration></note>",
        "<barline><repeat direction='backward'/></barline>",
        "<sound dacapo='yes'/>",
        "<note><unpitched/><duration>1</duration></note>",
        "<attributes><transpose number='2'><chromatic>2</chromatic></transpose></attributes>",
        "<note><grace/><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration></note>",
        "<sound tempo='120'/><sound tempo='90'/>",
        ("<attributes><divisions>1</divisions><time><beats>1</beats><beat-type>4</beat-type></time></attributes>"
        "<note><rest/><duration>2</duration></note>"),
        "<note attack='2'><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration></note>",
        "<note id='a' id='b'/>",
        "<note></rest>",
        "<note>bad &amp</note>",
        "<note>bad &#xD800;</note>",
        "<note>bad &#x110000;</note>",
        "<note>bad \xc0\x80</note>",
        "<note>bad \x01</note>",
        "<note><!-- bad -- comment --></note>",
        "<?xml version='1.0'?><note/>",
        "<note xmlns='urn:unknown'/>",
        "<direction><offset>-1</offset><sound tempo='120'/></direction>",
        "<attributes><divisions>1</divisions></attributes><sound><offset>5</offset></sound><note><rest/><duration>1</duration></note>"
    };
    size_t i;
    for (i = 0U; i < sizeof(bodies)/sizeof(bodies[0]); i++) {
        char *xml = document(bodies[i]);
        if (xml != NULL) { reject(xml, NULL, NULL); free(xml); }
    }
    reject("<!DOCTYPE score-partwise [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><score-partwise/>", NULL, "internal DTD");
    reject("<score-timewise/>", NULL, "timewise");
    reject("PKfake-archive", NULL, "ZIP");
    reject("<?xml version='1.0' encoding='UTF-16'?><score-partwise/>", NULL, "UTF-8");
    reject("<?xml version='1.0' encoding='UTF-16' note='UTF-8'?><score-partwise/>", NULL, "UTF-8");
    reject("<?xml version='1.1'?><score-partwise/>", NULL, "declaration");
    reject("<?XML version='1.0'?><score-partwise/>", NULL, "declaration");
    reject("<?xml encoding='UTF-8'?><score-partwise/>", NULL, "declaration");
    hwa_musicxml_options_default(NULL);
    hwa_musicxml_score_free(NULL);
}

int main(void)
{
    simple_and_limits(); timeline(); rounded_tuplets(); malformed();
    return failures != 0;
}
