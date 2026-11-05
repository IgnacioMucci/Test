/*
 * compressors.c - Puente (shim) para comparar LZ78 y TAMP con el MISMO método.
 *
 * LZ78 se incluye tal cual
 * (solo se renombra su main) y TAMP se compila desde su librería en C.
 *
 * Funciones exportadas (todas devuelven bytes escritos, o <0 si hay error):
 *   lz78_compress_mem / lz78_decompress_mem
 *   tamp_compress_mem / tamp_decompress_mem
 *   bench_*  -> repiten la operación N veces y devuelven ns por llamada
 *   *_state_bytes -> RAM estática/stack que necesita cada algoritmo
 *
 * Requiere Linux/WSL (usa fmemopen y open_memstream de POSIX.1-2008).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ---- LZ78 original, sin tocar: solo renombramos su main() ----------- */
#define main lz78_cli_main
#include "lz78.c"
#undef main

/* ---- TAMP (librería C oficial) -------------------------------------- */
#include "tamp/c_src/compressor.h"
#include "tamp/c_src/decompressor.h"

#define EXPORT __attribute__((visibility("default")))

/* =================== LZ78 sobre memoria =========================== */
EXPORT long lz78_compress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    if (n == 0) return 0;                       /* LZ78 no emite nada para vacío */
    FILE *fi = fmemopen((void *)in, n, "rb");
    char *buf = NULL; size_t len = 0;
    FILE *fo = open_memstream(&buf, &len);
    if (!fi || !fo) { if (fi) fclose(fi); if (fo) fclose(fo); free(buf); return -2; }
    int rc = compress(fi, fo);
    fclose(fi); fclose(fo);                     /* fclose(fo) actualiza buf/len */
    if (rc != 0 || len > cap) { free(buf); return -1; }
    memcpy(out, buf, len);
    free(buf);
    return (long)len;
}

EXPORT long lz78_decompress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    if (n == 0) return 0;
    FILE *fi = fmemopen((void *)in, n, "rb");
    char *buf = NULL; size_t len = 0;
    FILE *fo = open_memstream(&buf, &len);
    if (!fi || !fo) { if (fi) fclose(fi); if (fo) fclose(fo); free(buf); return -2; }
    int rc = decompress(fi, fo);
    fclose(fi); fclose(fo);
    if (rc != 0 || len > cap) { free(buf); return -1; }
    memcpy(out, buf, len);
    free(buf);
    return (long)len;
}

/* =================== TAMP sobre memoria =========================== */
EXPORT long tamp_compress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                              int window, int literal)
{
    unsigned char win[1u << 15];
    TampConf conf = {0};
    conf.window = window;
    conf.literal = literal;
    conf.extended = 1;                          /* igual que tamp.compress() de Python */
    TampCompressor c;
    if (tamp_compressor_init(&c, &conf, win) != TAMP_OK) return -3;
    size_t written = 0, consumed = 0;
    tamp_res r = tamp_compressor_compress_and_flush(&c, out, cap, &written, in, n,
                                                    &consumed, false);
    if (r != TAMP_OK || consumed != n) return -1;
    return (long)written;
}

EXPORT long tamp_decompress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                                int window)
{
    unsigned char win[1u << 15];
    TampDecompressor d;
    if (tamp_decompressor_init(&d, NULL, win, (uint8_t)window) != TAMP_OK) return -3;
    size_t written = 0, consumed = 0;
    tamp_res r = tamp_decompressor_decompress(&d, out, cap, &written, in, n, &consumed);
    if (r < 0) return -1;
    return (long)written;
}

/* =================== Cronómetro =================================== */
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* codec: 0 = LZ78, 1 = TAMP. Devuelve ns promedio por llamada. */
EXPORT double bench_compress(int codec, const uint8_t *in, size_t n, int window,
                             int literal, int iters)
{
    static uint8_t out[1 << 20];
    volatile long sink = 0;
    double t0 = now_ns();
    for (int i = 0; i < iters; i++)
        sink += codec == 0 ? lz78_compress_mem(in, n, out, sizeof out)
                           : tamp_compress_mem(in, n, out, sizeof out, window, literal);
    double t1 = now_ns();
    (void)sink;
    return (t1 - t0) / iters;
}

EXPORT double bench_decompress(int codec, const uint8_t *comp, size_t cn, int window,
                               int iters)
{
    static uint8_t out[1 << 20];
    volatile long sink = 0;
    double t0 = now_ns();
    for (int i = 0; i < iters; i++)
        sink += codec == 0 ? lz78_decompress_mem(comp, cn, out, sizeof out)
                           : tamp_decompress_mem(comp, cn, out, sizeof out, window);
    double t1 = now_ns();
    (void)sink;
    return (t1 - t0) / iters;
}

/* =================== Huella de memoria (RAM) ======================= */
/* LZ78: arrays estáticos del código original (+ buffer de 64 KiB del decoder). */
EXPORT size_t lz78_compressor_ram(void)   { return sizeof(parent_of) + sizeof(char_of) + sizeof(table); }
EXPORT size_t lz78_decompressor_ram(void) { return sizeof(parent_of) + sizeof(char_of) + DICT_SIZE; }
/* TAMP: struct de estado + ventana de (1 << window) bytes. */
EXPORT size_t tamp_compressor_ram(int window)   { return sizeof(TampCompressor) + ((size_t)1 << window); }
EXPORT size_t tamp_decompressor_ram(int window) { return sizeof(TampDecompressor) + ((size_t)1 << window); }
