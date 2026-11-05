/*
 * lz78.c - LZ78 compressor / decompressor
 *
 * Build:  gcc -O2 -Wall -o lz78 lz78.c
 * Usage:  ./lz78 c input output     (compress)
 *         ./lz78 d input output     (decompress)
 *
 * Format: a sequence of 3-byte tokens (index, char):
 *   - index: 16-bit little-endian dictionary index (0 = empty phrase)
 *   - char : the byte that extends that phrase
 * Each token outputs dict[index] + char and adds that string as a new
 * dictionary entry. When the dictionary fills (65536 entries) both sides
 * reset it to empty, so memory stays bounded for any input size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define DICT_BITS 16
#define DICT_SIZE (1u << DICT_BITS)        /* entries, including root 0 */
#define HASH_BITS (DICT_BITS + 1)
#define HASH_SIZE (1u << HASH_BITS)        /* load factor <= 0.5 */

/* ---------------------------------------------------------------- */
/* Shared dictionary: each entry is (parent phrase, last byte).      */
/* ---------------------------------------------------------------- */
static uint16_t parent_of[DICT_SIZE];
static uint8_t  char_of[DICT_SIZE];
static uint32_t next_code;

/* ---------------------------------------------------------------- */
/* Encoder: hash table maps (parent, byte) -> child code.            */
/* ---------------------------------------------------------------- */
typedef struct {
    uint32_t key;    /* (parent << 8) | byte */
    uint16_t code;
    uint8_t  used;
} Slot;

static Slot table[HASH_SIZE];

static void enc_reset(void)
{
    memset(table, 0, sizeof table);
    next_code = 1;
}

static uint32_t hash_key(uint32_t key)
{
    return (key * 2654435761u) >> (32 - HASH_BITS);
}

/* Returns slot index; slot is either the match or the empty place for it. */
static uint32_t find_slot(uint32_t key)
{
    uint32_t i = hash_key(key);
    while (table[i].used && table[i].key != key)
        i = (i + 1) & (HASH_SIZE - 1);
    return i;
}

static int write_token(FILE *out, uint32_t index, int ch)
{
    unsigned char t[3] = { index & 0xFF, (index >> 8) & 0xFF, (unsigned char)ch };
    return fwrite(t, 1, 3, out) == 3 ? 0 : -1;
}

static int compress(FILE *in, FILE *out)
{
    uint32_t cur = 0;   /* current phrase (0 = empty) */
    int ch;

    enc_reset();
    while ((ch = getc(in)) != EOF) {
        uint32_t key = (cur << 8) | (uint32_t)ch;
        uint32_t s = find_slot(key);

        if (table[s].used) {          /* phrase+ch already known: extend */
            cur = table[s].code;
            continue;
        }

        /* New phrase: emit token and add it to the dictionary. */
        if (write_token(out, cur, ch)) return -1;

        table[s].used = 1;
        table[s].key  = key;
        table[s].code = (uint16_t)next_code;
        parent_of[next_code] = (uint16_t)cur;
        char_of[next_code]   = (uint8_t)ch;
        if (++next_code == DICT_SIZE)
            enc_reset();
        cur = 0;
    }

    /* Input ended in the middle of a known phrase: emit it as
       (its parent, its last byte), which reproduces it exactly. */
    if (cur != 0 && write_token(out, parent_of[cur], char_of[cur]))
        return -1;

    return ferror(in) ? -1 : 0;
}

/* ---------------------------------------------------------------- */
/* Decoder                                                           */
/* ---------------------------------------------------------------- */
static int decompress(FILE *in, FILE *out)
{
    static uint8_t buf[DICT_SIZE];   /* a phrase is shorter than DICT_SIZE */
    unsigned char t[3];
    size_t n;

    next_code = 1;
    while ((n = fread(t, 1, 3, in)) == 3) {
        uint32_t index = t[0] | ((uint32_t)t[1] << 8);
        uint8_t  ch    = t[2];
        size_t   len   = 0;

        if (index >= next_code) {
            fprintf(stderr, "lz78: corrupt input (bad index %u)\n", index);
            return -1;
        }

        /* Walk up the tree, then write the phrase in forward order. */
        for (uint32_t c = index; c != 0; c = parent_of[c])
            buf[len++] = char_of[c];
        while (len > 0)
            if (putc(buf[--len], out) == EOF) return -1;
        if (putc(ch, out) == EOF) return -1;

        parent_of[next_code] = (uint16_t)index;
        char_of[next_code]   = ch;
        if (++next_code == DICT_SIZE)
            next_code = 1;            /* mirror the encoder's reset */
    }

    if (n != 0) {
        fprintf(stderr, "lz78: corrupt input (truncated token)\n");
        return -1;
    }
    return ferror(in) ? -1 : 0;
}

/* ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "c") && strcmp(argv[1], "d"))) {
        fprintf(stderr, "usage: %s c|d input output\n", argv[0]);
        return 2;
    }

    FILE *in = fopen(argv[2], "rb");
    if (!in) { perror(argv[2]); return 1; }
    FILE *out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); fclose(in); return 1; }

    int rc = (argv[1][0] == 'c') ? compress(in, out) : decompress(in, out);

    fclose(in);
    if (fclose(out) != 0) rc = -1;
    if (rc) { fprintf(stderr, "lz78: failed\n"); return 1; }
    return 0;
}
