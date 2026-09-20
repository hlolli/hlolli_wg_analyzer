#include "internal.h"
#include "musicxml_zip.h"

#include <stdlib.h>
#include <string.h>

typedef struct Bits {
    const unsigned char *data;
    size_t size, byte;
    unsigned bit;
    int failed;
} Bits;

typedef struct Huffman {
    unsigned count[16], symbols[288];
} Huffman;

static unsigned u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8U); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)u16(p) | ((uint32_t)u16(p+2) << 16U); }

static unsigned bits(Bits *b, unsigned n)
{
    unsigned result = 0U, i;
    for (i = 0U; i < n; i++) {
        if (b->byte == b->size) { b->failed = 1; return 0U; }
        result |= (((unsigned)b->data[b->byte] >> b->bit) & 1U) << i;
        if (++b->bit == 8U) { b->bit = 0U; b->byte++; }
    }
    return result;
}

static int tree(Huffman *h, const unsigned *lengths, unsigned n)
{
    unsigned i, offsets[16] = {0U};
    int left = 1;
    memset(h, 0, sizeof(*h));
    for (i = 0U; i < n; i++) {
        if (lengths[i] > 15U) return -1;
        h->count[lengths[i]]++;
    }
    for (i = 1U; i <= 15U; i++) {
        left = left*2-(int)h->count[i];
        if (left < 0) return -1;
        if (i < 15U) offsets[i+1U] = offsets[i]+h->count[i];
    }
    /* RFC 1951 permits a one-symbol distance tree (and an unused empty one). */
    if (left != 0 && (n-h->count[0] > 1U || (n-h->count[0] == 1U && h->count[1] != 1U))) return -1;
    for (i = 0U; i < n; i++) if (lengths[i] != 0U) h->symbols[offsets[lengths[i]]++] = i;
    return 0;
}

static int symbol(Bits *b, const Huffman *h)
{
    unsigned code = 0U, first = 0U, index = 0U, len;
    for (len = 1U; len <= 15U; len++) {
        code |= bits(b, 1U);
        if (b->failed) return -1;
        if (code >= first && code-first < h->count[len]) return (int)h->symbols[index+code-first];
        index += h->count[len]; first = (first+h->count[len]) << 1U; code <<= 1U;
    }
    return -1;
}

static int inflate_bytes(const unsigned char *data, size_t size, unsigned char *out, size_t length)
{
    static const unsigned order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    static const unsigned len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    Bits b = {data, size, 0U, 0U, 0};
    size_t written = 0U;
    unsigned final;
    do {
        unsigned type, lengths[320] = {0U}, i, literals = 288U, distances = 32U;
        Huffman lit, dist;
        final = bits(&b, 1U); type = bits(&b, 2U);
        if (b.failed || type == 3U) return -1;
        if (type == 0U) {
            unsigned n, complement;
            if (b.bit != 0U) { b.bit = 0U; b.byte++; }
            n = bits(&b, 16U); complement = bits(&b, 16U);
            if (b.failed || (n ^ complement) != 65535U || n > length-written || n > size-b.byte) return -1;
            memcpy(out+written, data+b.byte, n); written += n; b.byte += n;
            continue;
        }
        if (type == 1U) {
            for (i = 0U; i < 288U; i++) lengths[i] = i < 144U ? 8U : i < 256U ? 9U : i < 280U ? 7U : 8U;
            for (i = 288U; i < 320U; i++) lengths[i] = 5U;
        } else {
            unsigned codes, code_lengths[19] = {0U}, count = 0U;
            Huffman code_tree;
            literals = bits(&b, 5U)+257U; distances = bits(&b, 5U)+1U; codes = bits(&b, 4U)+4U;
            if (b.failed || literals > 286U) return -1;
            for (i = 0U; i < codes; i++) code_lengths[order[i]] = bits(&b, 3U);
            if (b.failed || tree(&code_tree, code_lengths, 19U) != 0) return -1;
            while (count < literals+distances) {
                int s = symbol(&b, &code_tree);
                unsigned repeat, value;
                if (s < 0) return -1;
                if (s < 16) { lengths[count++] = (unsigned)s; continue; }
                if (s == 16) {
                    if (count == 0U) return -1;
                    repeat = bits(&b, 2U)+3U; value = lengths[count-1U];
                } else { repeat = bits(&b, s == 17 ? 3U : 7U)+(s == 17 ? 3U : 11U); value = 0U; }
                if (b.failed || repeat > literals+distances-count) return -1;
                while (repeat-- != 0U) lengths[count++] = value;
            }
        }
        if (lengths[256] == 0U || tree(&lit, lengths, literals) != 0 ||
            tree(&dist, lengths+literals, distances) != 0) return -1;
        for (;;) {
            int s = symbol(&b, &lit), d;
            unsigned n, distance;
            if (s < 0 || s > 285) return -1;
            if (s == 256) break;
            if (s < 256) {
                if (written == length) return -1;
                out[written++] = (unsigned char)s; continue;
            }
            n = len_base[s-257]+bits(&b, len_extra[s-257]);
            d = symbol(&b, &dist);
            if (d < 0 || d > 29) return -1;
            distance = dist_base[d]+bits(&b, dist_extra[d]);
            if (b.failed || distance > written || n > length-written) return -1;
            while (n-- != 0U) { out[written] = out[written-distance]; written++; }
        }
    } while (!final);
    return written == length && b.byte+(b.bit != 0U ? 1U : 0U) == size ? 0 : -1;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    for (i = 0U; i < size; i++) {
        unsigned bit;
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++) crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
    }
    return crc ^ UINT32_MAX;
}

int hwa_musicxml_zip_member(const unsigned char *zip, size_t size, const char *name,
                           uint64_t limit, unsigned char **data, size_t *length,
                           char *error, size_t error_size)
{
    size_t eocd, first, end, p, found = SIZE_MAX, name_size = strlen(name), i;
    unsigned entries;
    *data = NULL; *length = 0U;
    if (size < 22U || name_size == 0U || name[0] == '/' || strchr(name, '\\') != NULL || strchr(name, ':') != NULL) goto bad;
    for (i = 0U; i < name_size; ) {
        size_t start = i;
        while (i < name_size && name[i] != '/') i++;
        if (i == start || (i-start == 1U && name[start] == '.') ||
            (i-start == 2U && name[start] == '.' && name[start+1U] == '.')) goto bad;
        if (i < name_size) i++;
    }
    eocd = size-22U;
    for (;;) {
        if (u32(zip+eocd) == UINT32_C(0x06054b50) && u16(zip+eocd+20U) == size-eocd-22U) break;
        if (eocd == 0U || size-eocd > 65557U) goto bad;
        eocd--;
    }
    entries = u16(zip+eocd+10U);
    if (u16(zip+eocd+4U) || u16(zip+eocd+6U) || entries != u16(zip+eocd+8U) || entries == 65535U) goto bad;
    first = u32(zip+eocd+16U);
    if (first > eocd || u32(zip+eocd+12U) != eocd-first) goto bad;
    end = eocd; p = first;
    for (i = 0U; i < entries; i++) {
        size_t n, extra, comment;
        if (end-p < 46U || u32(zip+p) != UINT32_C(0x02014b50)) goto bad;
        n = u16(zip+p+28U); extra = u16(zip+p+30U); comment = u16(zip+p+32U);
        if (n+extra+comment > end-p-46U) goto bad;
        if (n == name_size && memcmp(zip+p+46U, name, n) == 0) {
            if (found != SIZE_MAX) goto bad;
            found = p;
        }
        p += 46U+n+extra+comment;
    }
    if (p != end || found == SIZE_MAX) goto bad;
    {
        unsigned flags = u16(zip+found+8U), method = u16(zip+found+10U);
        size_t compressed = u32(zip+found+20U), uncompressed = u32(zip+found+24U);
        size_t local = u32(zip+found+42U), offset, n, extra;
        uint32_t crc = u32(zip+found+16U);
        unsigned char *out;
        if ((flags & ~0x080eU) || (method != 0U && method != 8U) || u16(zip+found+34U) ||
            uncompressed == UINT32_MAX || compressed == UINT32_MAX || uncompressed == SIZE_MAX ||
            local > first || first-local < 30U || u32(zip+local) != UINT32_C(0x04034b50)) goto bad;
        if ((uint64_t)uncompressed+1U > limit) {
            hwa_set_error(error, error_size, "MusicXML ZIP: expanded member exceeds byte/work limit"); return -1;
        }
        n = u16(zip+local+26U); extra = u16(zip+local+28U);
        if (n+extra > first-local-30U || n != name_size || memcmp(zip+local+30U, name, n) != 0 ||
            u16(zip+local+6U) != flags || u16(zip+local+8U) != method) goto bad;
        if (!(flags & 8U) && (u32(zip+local+14U) != crc || u32(zip+local+18U) != compressed ||
                             u32(zip+local+22U) != uncompressed)) goto bad;
        offset = local+30U+n+extra;
        if (compressed > first-offset) goto bad;
        out = malloc(uncompressed+1U);
        if (out == NULL) { hwa_set_error(error, error_size, "MusicXML ZIP: out of memory"); return -1; }
        if (method == 0U && compressed == uncompressed) memcpy(out, zip+offset, uncompressed);
        else if (method != 8U || inflate_bytes(zip+offset, compressed, out, uncompressed) != 0) { free(out); goto bad; }
        if (crc32_bytes(out, uncompressed) != crc) { free(out); goto bad; }
        out[uncompressed] = 0U; *data = out; *length = uncompressed; return 0;
    }
bad:
    hwa_set_error(error, error_size, "MusicXML ZIP: malformed, missing or unsupported member (stored/DEFLATE, single-disk ZIP required)");
    return -1;
}
