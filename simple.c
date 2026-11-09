/*
 * simple.c - Version minima: LZ78 y TAMP juntos, un CSV y tests simples.
 *
 *   ./simple test [--size N]            tests con datos ejemplos
 *   ./simple ARCHIVO [opciones]         comprime el archivo y tambien sus filas
 *   ./simple ARCHIVO.csv --flare [opc]   para utilizar json y formato
 *
 * Opciones (modo archivo):
 *   --rows K      agrupar K filas por mensaje (def. 1) -> simula tramas chicas
 *   --show S      cuantos mensajes mostrar en la tabla (def. 10)
 *   --no-header   la primera linea del CSV es un dato, no un encabezado
 * Opciones generales:
 *   -w W -l L     ventana (8..15) y literal de TAMP (def. 10 y 8)
 *   --size N      largo de los casos del modo test (def. 1000)
 *
 * Modo FlareSense (--flare):
 *   csv_to_json.py       CSV -> JSON completo
 *   encode_flaresense.py JSON completo -> formato del jefe ("bare-body")
 * Para 1..N muestras arma las dos tramas (JSON y formato), las comprime con
 * LZ78 y TAMP y las compara contra el JSON original. Despues calcula la TRAMA
 * MINIMA (primer N donde cada combinacion empieza a ahorrar) y compara la
 * COMPOSICION de la trama (que sensores lleva).
 *   --tools DIR      carpeta de los .py (def. "." = donde corres ./simple)
 *   --python CMD     interprete (def. python3)
 *   --max-n N        barrido de 1..N muestras (def. 30)
 *   --comp-n N       muestras de las tramas del cuadro de composicion (def. 10)
 *   --offset K       saltear K filas de datos despues del encabezado (def. 0)
 *   --json-completo  el JSON lleva fw/com_mode/rssi (def.: JSON minimo de la
 *                    minuta: nodeId, delta_t, readings)
 *
 * Para cada entrada: comprime con LZ78 y con TAMP, descomprime, compara con el
 * original y muestra tamaños. Ahorro negativo = el compresor AGRANDO el dato.
 * Codigo de salida: 0 si todo OK, 1 si algun round-trip falla, 2 si se uso mal.
 *
 * Los algoritmos estan en lz78.c y tamp/ (originales); compressors.c es el
 * puente que los expone con una interfaz sobre memoria.
 */
#define _POSIX_C_SOURCE 200809L          /* popen, mkstemp, strcasecmp */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

long lz78_compress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
long lz78_decompress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
long tamp_compress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                       int window, int literal);
long tamp_decompress_mem(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                         int window);

static int W = 10, L = 8;           /* parametros de TAMP */

typedef struct { long lz, tp; int ok_lz, ok_tp; } Res;

/* Comprime con los dos, descomprime y compara. Devuelve 0 si ambos coinciden. */
static int probar(const uint8_t *d, size_t n, Res *r)
{
    size_t cap = n * 3 + 64, dcap = n + 1024;
    uint8_t *z = malloc(cap), *back = malloc(dcap);
    if (!z || !back) { fprintf(stderr, "sin memoria\n"); exit(1); }

    r->lz = lz78_compress_mem(d, n, z, cap);
    r->ok_lz = r->lz >= 0 &&
               lz78_decompress_mem(z, (size_t)r->lz, back, dcap) == (long)n &&
               memcmp(back, d, n) == 0;

    r->tp = tamp_compress_mem(d, n, z, cap, W, L);
    r->ok_tp = r->tp >= 0 &&
               tamp_decompress_mem(z, (size_t)r->tp, back, dcap, W) == (long)n &&
               memcmp(back, d, n) == 0;

    free(z); free(back);
    return !(r->ok_lz && r->ok_tp);
}

static void celda(size_t n, long z, int ok)
{
    if (z < 0)        printf("%-20s", "ERROR");
    else if (n == 0)  printf("%6ld %8s %-4s", z, "-", ok ? "OK" : "FALLA");
    else              printf("%6ld %+7.1f%% %-4s", z, (1.0 - (double)z / (double)n) * 100.0,
                             ok ? "OK" : "FALLA");
}

static void encabezado(void)
{
    printf("\n%-22s %7s | %-20s | %-20s\n", "caso", "bytes", "LZ78 (sale/ahorro/RT)", "TAMP (idem)");
    printf("---------------------------------------------------------------------------\n");
}

static void fila(const char *nombre, size_t n, const Res *r)
{
    printf("%-22s %7zu | ", nombre, n);
    celda(n, r->lz, r->ok_lz);
    printf(" | ");
    celda(n, r->tp, r->ok_tp);
}

/* ======================= MODO TEST ======================================= */

static uint32_t semilla = 12345;               /* azar reproducible */
static uint32_t azar(void) { semilla = semilla * 1103515245u + 12345u; return semilla >> 16; }

static int corre_caso(const char *nombre, const uint8_t *d, size_t n, int debe_comprimir)
{
    Res r;
    int mal = probar(d, n, &r);
    fila(nombre, n, &r);
    if (!mal && debe_comprimir && (r.lz * 2 >= (long)n || r.tp * 2 >= (long)n)) mal = 1;
    printf(" -> %s\n", mal ? "FALLA" : "PASS");
    return mal;
}

static int modo_test(size_t N)
{
    int fallas = 0;
    size_t i, h = N / 2;
    uint8_t *b = malloc(N + 32);
    if (!b) return 1;

    printf("== Tests simples (%zu bytes) ==\n", N);
    printf("PASS = round-trip identico (y, en todo_a/todo_b, sale menos de la mitad)\n");
    encabezado();

    fallas += corre_caso("vacio", b, 0, 0);
    b[0] = 'a';
    fallas += corre_caso("un_byte", b, 1, 0);

    memset(b, 'a', N);                         
    fallas += corre_caso("todo_a", b, N, 1);
    memset(b, 'b', N);
    fallas += corre_caso("todo_b", b, N, 1);

    memset(b, 'a', h); memset(b + h, 'b', N - h);
    fallas += corre_caso("50a_50b_bloques", b, N, 0);

    for (i = 0; i < N; i++) b[i] = (i % 2) ? 'b' : 'a';
    fallas += corre_caso("50a_50b_alternado", b, N, 0);

    for (i = 0; i < N; i++) b[i] = (azar() & 1) ? 'b' : 'a';
    fallas += corre_caso("50a_50b_azar", b, N, 0);

    for (i = 0; i < N; i++) b[i] = (uint8_t)('0' + i % 10);
    fallas += corre_caso("digitos_repetidos", b, N, 0);

    for (i = 0, h = 0; h < N; i++) {            /* "1,2,3,4,..." */
        char t[16];
        int k = snprintf(t, sizeof t, "%zu,", i + 1);
        for (int j = 0; j < k && h < N; j++) b[h++] = (uint8_t)t[j];
    }
    fallas += corre_caso("enteros_1,2,3...", b, N, 0);

    for (h = 0; h < N; ) {                      /* enteros al azar 0..999 */
        char t[16];
        int k = snprintf(t, sizeof t, "%u,", azar() % 1000);
        for (int j = 0; j < k && h < N; j++) b[h++] = (uint8_t)t[j];
    }
    fallas += corre_caso("enteros_al_azar", b, N, 0);

    for (i = 0; i < N; i++) b[i] = (uint8_t)(azar() & 0xFF);
    fallas += corre_caso("bytes_al_azar", b, N, 0);

    free(b);
    printf("\nRESULTADO: %s\n", fallas ? "HAY FALLAS" : "todo OK");
    return fallas ? 1 : 0;
}

/* ======================= MODO ARCHIVO / CSV ============================== */

static uint8_t *leer_todo(const char *ruta, size_t *n)
{
    FILE *f = fopen(ruta, "rb");
    if (!f) { perror(ruta); return NULL; }
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        len += fread(buf + len, 1, cap - len, f);
        if (len < cap) break;
        cap *= 2;
        buf = realloc(buf, cap);
    }
    fclose(f);
    if (buf) *n = len;
    return buf;
}

static void vista_previa(const uint8_t *d, size_t n)
{
    size_t i, m = n < 100 ? n : 100;
    printf("    ");
    for (i = 0; i < m; i++) putchar(d[i] == '\n' ? '|' : d[i]);
    printf(n > m ? " ...\n" : "\n");
}

typedef struct {
    uint8_t *msg;                     /* mensaje que se esta armando */
    size_t ml, enbloque, K;
    size_t total_n, nmsg;
    long total_lz, total_tp, mal_lz, mal_tp;
    int fallas, mostrar;
} Ctx;

/* Comprime el mensaje armado hasta ahora (si hay algo) y lo suma a los totales. */
static void emitir(Ctx *c)
{
    Res r;
    if (c->enbloque == 0) return;
    c->nmsg++;
    c->fallas += probar(c->msg, c->ml, &r);
    if (r.lz >= 0) c->total_lz += r.lz;
    if (r.tp >= 0) c->total_tp += r.tp;
    c->mal_lz += !r.ok_lz;
    c->mal_tp += !r.ok_tp;
    c->total_n += c->ml;
    if ((int)c->nmsg <= c->mostrar) {
        char nom[32];
        snprintf(nom, sizeof nom, "mensaje_%zu", c->nmsg);
        fila(nom, c->ml, &r); printf("\n");
        vista_previa(c->msg, c->ml);
    }
    c->ml = 0; c->enbloque = 0;
}

static int modo_archivo(const char *ruta, int K, int mostrar, int con_header)
{
    size_t n = 0, i = 0;
    uint8_t *d = leer_todo(ruta, &n);
    if (!d) return 1;

    Res r;
    int fallas = probar(d, n, &r);

    printf("== Archivo: %s (%zu bytes) ==\n", ruta, n);
    encabezado();
    fila("archivo_completo", n, &r);
    printf("\n");

    /* Filas: cada K lineas forman un mensaje que se comprime por separado. */
    Ctx c;
    memset(&c, 0, sizeof c);
    c.msg = malloc(n + 1);
    if (!c.msg) return 1;
    c.K = (size_t)K; c.mostrar = mostrar;
    int primera = 1;

    printf("\n-- Mensajes de %d fila(s)%s (se muestran los primeros %d) --\n",
           K, con_header ? ", sin la primera linea (encabezado)" : "", mostrar);
    encabezado();

    while (i < n) {
        size_t ini = i, fin = i;
        while (fin < n && d[fin] != '\n') fin++;
        i = fin + 1;
        size_t len = fin - ini;
        if (len > 0 && d[ini + len - 1] == '\r') len--;      /* CRLF */
        if (primera && con_header) { primera = 0; continue; } /* encabezado */
        primera = 0;
        if (len == 0) continue;                               /* linea vacia */

        if (c.enbloque > 0) c.msg[c.ml++] = '\n';
        memcpy(c.msg + c.ml, d + ini, len);
        c.ml += len; c.enbloque++;
        if (c.enbloque == c.K) emitir(&c);
    }
    emitir(&c);                                               /* bloque incompleto final */
    fallas += c.fallas;

    if (c.nmsg == 0) {
        printf("(no se encontraron filas de datos)\n");
    } else {
        Res t = { c.total_lz, c.total_tp, c.mal_lz == 0, c.mal_tp == 0 };
        char nom[40];
        printf("---------------------------------------------------------------------------\n");
        snprintf(nom, sizeof nom, "TOTAL (%zu mensajes)", c.nmsg);
        fila(nom, c.total_n, &t); printf("\n");
    }
    printf("\nRESULTADO: %s\n", fallas ? "HAY FALLAS (ver filas con ERROR o FALLA)" : "todo OK");
    free(c.msg); free(d);
    return fallas ? 1 : 0;
}

/* ======================= MODO FLARESENSE ================================= */
/*
 * Cadena: CSV --csv_to_json.py--> JSON completo --encode_flaresense.py--> formato.
 * Los .py se llaman como programas externos (popen);
 */

static char tools_dir[512] = ".";
static char python_cmd[64] = "python3";
static char out_dir[512]    = "";

/* Mete s entre comillas simples para el shell. */
static void sh_quote(char *dst, size_t cap, const char *s)
{
    size_t k = 0;
    dst[k++] = '\'';
    for (; *s && k + 6 < cap; s++) {
        if (*s == '\'') { memcpy(dst + k, "'\\''", 4); k += 4; }
        else dst[k++] = *s;
    }
    dst[k++] = '\'';
    dst[k] = 0;
}

/* Corre un comando y devuelve todo su stdout (terminado en 0). NULL si falla. */
static char *capturar(const char *cmd, size_t *len)
{
    FILE *p = popen(cmd, "r");
    if (!p) { perror("popen"); return NULL; }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n - 1, p);
        if (n < cap - 1) break;
        cap *= 2;
        buf = realloc(buf, cap);
    }
    int st = pclose(p);
    if (!buf) return NULL;
    if (st != 0) { free(buf); return NULL; }     /* el .py ya explico el error por stderr */
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}


static void quitar_blancos(char *s, size_t *n)   /* JSON compacto: sin espacios fuera de comillas */
{
    size_t i, k = 0;
    int en_str = 0;
    for (i = 0; i < *n; i++) {
        char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) en_str = !en_str;
        if (!en_str && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) continue;
        s[k++] = c;
    }
    s[k] = 0; *n = k;
}

static void quitar_clave(char *s, size_t *n, const char *clave)   /* saca  "clave":valor,  */
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", clave);
    char *a = strstr(s, pat);
    if (!a) return;
    char *e = a + strlen(pat);
    if (*e == '"') { e++; while (*e && *e != '"') e++; if (*e) e++; }
    else while (*e && *e != ',' && *e != '}') e++;
    if (*e == ',') e++;
    memmove(a, e, strlen(e) + 1);
    *n = strlen(s);
}

static void renombrar_clave(char *s, size_t *n, const char *de, const char *a)
{
    char p1[64], p2[64];
    snprintf(p1, sizeof p1, "\"%s\":", de);
    snprintf(p2, sizeof p2, "\"%s\":", a);
    char *x = strstr(s, p1);
    if (!x) return;
    size_t l1 = strlen(p1), l2 = strlen(p2);
    memmove(x + l2, x + l1, strlen(x + l1) + 1);
    memcpy(x, p2, l2);
    *n = strlen(s);
}


typedef struct { char *json, *bare; size_t nj, nb; } Trama;

static void liberar(Trama *t) { free(t->json); free(t->bare); t->json = t->bare = NULL; }

/* Arma la trama de n muestras del CSV (a partir de la fila off). 0 = bien. */
static int armar_trama(const char *csv, long n, long off, int modo, Trama *t)
{
    char path[1100], q_py[1200], q_csv[1200], cmd[4096];
    memset(t, 0, sizeof *t);

    /* 1) CSV -> JSON completo (stdout de csv_to_json.py) */
    snprintf(path, sizeof path, "%s/csv_to_json.py", tools_dir);
    sh_quote(q_py, sizeof q_py, path);
    sh_quote(q_csv, sizeof q_csv, csv);
    snprintf(cmd, sizeof cmd, "%s %s %s -n %ld --offset %ld --indent 0",
             python_cmd, q_py, q_csv, n, off);
    t->json = capturar(cmd, &t->nj);
    if (!t->json) return 1;
        while (t->nj > 0 && (t->json[t->nj - 1] == '\n' || t->json[t->nj - 1] == '\r'))
        t->json[--t->nj] = 0;                    /* el \n final lo agrega print(), no es parte del JSON */

    /* 2) JSON -> formato (encode_flaresense.py acepta el JSON como literal) */
    snprintf(path, sizeof path, "%s/encode_flaresense.py", tools_dir);
    sh_quote(q_py, sizeof q_py, path);
    size_t ccap = t->nj * 4 + 4096;
    char *q_json = malloc(t->nj * 4 + 8), *big = malloc(ccap);
    if (!q_json || !big) { fprintf(stderr, "sin memoria\n"); exit(1); }
    sh_quote(q_json, t->nj * 4 + 8, t->json);
    snprintf(big, ccap, "%s %s %s", python_cmd, q_py, q_json);
    t->bare = capturar(big, &t->nb);
    free(q_json); free(big);
    if (!t->bare) { liberar(t); return 1; }
    while (t->nb > 0 && (t->bare[t->nb - 1] == '\n' || t->bare[t->nb - 1] == '\r'))
        t->bare[--t->nb] = 0;

    /* 3) variante de JSON contra la que se compara (el formato ya se genero del JSON crudo)
     *    modo 0 = crudo, 1 = compacto (sin espacios), 2 = minuta (sin metadata, con delta_t) */
    if (modo >= 1) quitar_blancos(t->json, &t->nj);
    if (modo == 2) {
        quitar_clave(t->json, &t->nj, "fw");
        quitar_clave(t->json, &t->nj, "com_mode");
        quitar_clave(t->json, &t->nj, "rssi");
        renombrar_clave(t->json, &t->nj, "timeDeltaM", "delta_t");
    }
    return 0;
}
static int contar_lecturas(const char *s, int *items, int *numeros)
{
    const char *p = strstr(s, "\"readings\"");
    int depth = 0, en_str = 0;
    *items = *numeros = 0;
    if (!p || !(p = strchr(p, '['))) return 0;
    for (; *p; p++) {
        char c = *p;
        if (en_str) {
            if (c == '\\' && p[1]) p++;
            else if (c == '"') en_str = 0;
            continue;
        }
        if (c == '"') { en_str = 1; continue; }
        if (c == '[' || c == '{') { depth++; continue; }
        if (c == ']' || c == '}') {
            depth--;
            if (depth == 1) (*items)++;
            if (depth == 0) break;
            continue;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char *q = p;
            while (*q && strchr("0123456789.-+eE", *q)) q++;
            if (!*q) break;                    /* numero cortado por el prefijo */
            (*numeros)++;
            if (depth == 1) (*items)++;
            p = q - 1;
        }
    }
    return 1;
}
 

static const struct { int id; const char *nombre; const char *campos[8]; const char *aux[3]; } ESQ[] = {
    {   1, "ds18b20",         { "temperature", NULL },                                                     { "temperature_aux", NULL } },
    {   2, "sht3x",           { "temperature", "humidity", NULL },                                         { "temperature_aux", "humidity_aux", NULL } },
    {   3, "weather_station", { "wind_speed", "wind_gust", "wind_deg", "temperature", "humidity", "pressure", NULL }, { NULL } },
    {   4, "rain_gauge",      { "rain", NULL },                                                            { NULL } },
    {   7, "bh1750",          { "lux", NULL },                                                             { NULL } },
    {   9, "battery",         { "battery", NULL },                                                         { NULL } },
    { 130, "ds18b20_aux",     { "temperature", NULL },                                                     { NULL } },   /* 0x82 */
};

/* ---- evaluacion: las 4 combinaciones + las dos tramas sin comprimir ---- */

static void contar_valores(const char *s, int *series, int *valores)
{
    const char *p = strstr(s, "\"readings\"");
    int en_arr = 0, en_tok = 0;
    *series = *valores = 0;
    if (!p) return;
    for (p += 10; *p; p++) {
        char c = *p;
        if (c == '[')                { (*series)++; en_arr = 1; en_tok = 0; }
        else if (c == ']')           { en_arr = 0; en_tok = 0; }
        else if (c == ',' || c == ' ') en_tok = 0;
        else if (en_arr && !en_tok)  { en_tok = 1; (*valores)++; }
    }
}
 
/* Imprime los valores de una serie desde p hasta uno de los caracteres de 'fines' (o fin del
 * texto). Devuelve el puntero donde paro. Un valor sin terminador al final queda marcado (cortado). */
static const char *leer_serie(const char *p, const char *fines)
{
    char tok[40];
    size_t k = 0;
    int primero = 1;
    for (;; p++) {
        char c = *p;
        int fin = (c == 0) || strchr(fines, c) != NULL;
        if (c == ',' || fin) {
            tok[k] = 0;
            if (k) {
                printf("%s%s%s", primero ? "" : ", ", tok, c == 0 ? " (cortado)" : "");
                primero = 0;
            }
            k = 0;
            if (fin) return p;
        } else if (c != ' ' && k < sizeof tok - 1) {
            tok[k++] = c;
        }
    }
}
 
/* Desglosa un prefijo (o trama) en FORMATO: encabezado, sensores, campos y valores. */
static void anotar_formato(const char *s)
{
    size_t n = strlen(s), i;
    int hay_ws = strstr(s, "(3)[") != NULL;   /* si esta weather_station, sht3x/ds18b20 usan nombres _aux */
 
    printf("    nodeId  (3 chars): %.*s\n", (int)(n < 3 ? n : 3), s);
    if (n > 4) printf("    delta_t (5 digitos): %.*s\n", (int)(n - 4 < 5 ? n - 4 : 5), s + 4);
    if (n <= 10) { printf("    (el prefijo termina dentro del encabezado)\n"); return; }
    for (i = 10; i < n && s[i] == '('; ) {
        int sid = atoi(s + i + 1), f = 0, e = -1;
        const char *par = strchr(s + i, ')'), *nombre = "?";
        const char *const *campos = NULL;
        if (!par || par[1] != '[') { printf("    (sensor cortado: \"%s\")\n", s + i); return; }
        for (size_t k = 0; k < sizeof ESQ / sizeof ESQ[0]; k++)
            if (ESQ[k].id == sid) { e = (int)k; break; }
        if (e >= 0) {
            nombre = ESQ[e].nombre;
            campos = (hay_ws && ESQ[e].aux[0]) ? ESQ[e].aux : ESQ[e].campos;
        }
        printf("    sensor (%d) %s\n", sid, nombre);
        const char *p = par + 2;
        for (;;) {
            if (campos && campos[f]) printf("      %-16s = ", campos[f]);
            else                     printf("      campo %-10d = ", f);
            p = leer_serie(p, ";]");
            printf("\n");
            if (*p == ';') { p++; f++; continue; }
            break;
        }
        if (*p == ']') { p++; i = (size_t)(p - s); }
        else return;                                       /* prefijo termino adentro de este sensor */
    }
}
 
/* Desglosa un prefijo (o trama) JSON: metadatos y, por cada serie, los valores. */
static void anotar_json(const char *s)
{
    const char *r = strstr(s, "\"readings\"");
    if (!r) {
        printf("    todo metadatos (%zu B): nodeId, fw, timeDeltaM, com_mode, rssi; todavia no hay ningun valor de sensor\n", strlen(s));
        return;
    }
    printf("    metadatos: %zu B antes de \"readings\" (nodeId, fw, timeDeltaM, com_mode, rssi)\n", (size_t)(r - s));
    const char *p = strchr(r, '{');
    if (!p) return;
    for (p++; *p; ) {
        const char *k0 = strchr(p, '"'), *k1;
        if (!k0) return;
        k1 = strchr(k0 + 1, '"');
        if (!k1) { printf("      (nombre de serie cortado)\n"); return; }
        printf("      %-16.*s = ", (int)(k1 - k0 - 1), k0 + 1);
        p = strchr(k1, '[');
        if (!p) { printf("(todavia sin valores)\n"); return; }
        p = leer_serie(p + 1, "]");
        printf("\n");
        if (*p != ']') return;
        p++;
    }
}
 
/* Muestra el contenido de los primeros l bytes de d: texto, hex y desglose por campo. */
static void mostrar_prefijo(const char *nom, const uint8_t *d, size_t l, size_t una_muestra, int es_json)
{
    char *tmp = malloc(l + 1);
    if (!tmp) return;
    memcpy(tmp, d, l); tmp[l] = 0;
    printf("\n  %s: %zu B (una muestra completa mide %zu B)\n    texto: \"", nom, l, una_muestra);
    for (size_t i = 0; i < l; i++) {
        uint8_t c = d[i];
        if (c == '\n') fputs("\\n", stdout);
        else if (c >= 32 && c < 127) putchar(c);
        else printf("\\x%02x", c);
    }
    printf("\"\n");
    if (es_json) anotar_json(tmp); else anotar_formato(tmp);
    free(tmp);
}

typedef struct { size_t nj, nb; Res rj, rb; int mal; int series, valores; } Ev;
 
static int evaluar(const Trama *t, Ev *e)
{
    e->nj = t->nj; e->nb = t->nb;
    contar_valores(t->json, &e->series, &e->valores);    /* NUEVO */
    e->mal  = probar((const uint8_t *)t->json, t->nj, &e->rj);
    e->mal |= probar((const uint8_t *)t->bare, t->nb, &e->rb);
    return e->mal;
}

/* Combinacion c: 0 JSON+LZ78, 1 JSON+TAMP, 2 FORMATO+LZ78, 3 FORMATO+TAMP.
 * Devuelve tamaño de entrada y de salida. */
static void datos_comb(const Ev *e, int c, size_t *in, long *z)
{
    *in = c < 2 ? e->nj : e->nb;
    *z  = c == 0 ? e->rj.lz : c == 1 ? e->rj.tp : c == 2 ? e->rb.lz : e->rb.tp;
}

static int sale_menos(const Ev *e, int c)
{
    size_t in; long z;
    datos_comb(e, c, &in, &z);
    return z >= 0 && z < (long)in;
}

/* celda de 14 caracteres: bytes y ahorro respecto de 'base' ('!' = round-trip fallo) */
static void cel_flare(long z, int ok, size_t base)
{
    if (z < 0) { printf("%-16s", "ERROR"); return; }
    printf("%5ld B %+6.1f%%%c", z, base ? (1.0 - (double)z / (double)base) * 100.0 : 0.0,
           ok ? ' ' : '!');
}

static void encabezado_flare(const char *primera)
{
     printf("\n%-32s %7s | %-7s | %-16s | %-16s | %-16s | %-16s\n",
           primera, "JSON", "FORMATO", "JSON+LZ78", "JSON+TAMP", "FORMATO+LZ78", "FORMATO+TAMP");
    printf("-------------------------------------------------------------------------------------------------------------------------\n");}

static void fila_flare(const char *et, const Ev *e)
{
    printf("%-32s %5zu B | ", et, e->nj);
    printf("%5zu B | ", e->nb);
    cel_flare(e->rj.lz, e->rj.ok_lz, e->nj); printf(" | ");
    cel_flare(e->rj.tp, e->rj.ok_tp, e->nj); printf(" | ");
    cel_flare(e->rb.lz, e->rb.ok_lz, e->nb); printf(" | ");
    cel_flare(e->rb.tp, e->rb.ok_tp, e->nb); printf("\n");
}
static void guardar_archivo(const char *prefijo, const char *nombre, const char *ext,
                            const uint8_t *d, size_t n)
{
    char ruta[1200];
    snprintf(ruta, sizeof ruta, "%s/%s_%s.%s", out_dir, prefijo, nombre, ext);
    FILE *f = fopen(ruta, "wb");
    if (!f) { perror(ruta); return; }
    if (fwrite(d, 1, n, f) != n) perror(ruta);
    fclose(f);
}

static void guardar_evidencia(const char *prefijo, const Trama *t)
{
    static const char *nom[2] = { "json", "formato" };
    const uint8_t *dat[2] = { (const uint8_t *)t->json, (const uint8_t *)t->bare };
    size_t len[2] = { t->nj, t->nb };

    if (!out_dir[0]) return;
    for (int i = 0; i < 2; i++) {
        size_t cap = len[i] * 3 + 64;
        uint8_t *z = malloc(cap);
        if (!z) return;
        guardar_archivo(prefijo, nom[i], "txt", dat[i], len[i]);
        long a = lz78_compress_mem(dat[i], len[i], z, cap);
        if (a >= 0) guardar_archivo(prefijo, nom[i], "lz78", z, (size_t)a);
        long b = tamp_compress_mem(dat[i], len[i], z, cap, W, L);
        if (b >= 0) guardar_archivo(prefijo, nom[i], "tamp", z, (size_t)b);
        free(z);
    }
}

static int es_hito(int n, int maxn)
{
    static const int h[] = { 1, 2, 3, 4, 5, 10, 15, 20, 25, 30, 40, 50, 75, 100, 150, 200 };
    if (n == maxn) return 1;
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++) if (h[i] == n) return 1;
    return 0;
}

/* ---- TRAMA MINIMA ---- */

static void trama_minima(const Ev *ev, int maxn)
{
    static const char *nom[4] = { "JSON + LZ78", "JSON + TAMP", "FORMATO + LZ78", "FORMATO + TAMP" };
 
    printf("\n-- TRAMA MINIMA: primera cantidad de muestras donde la salida es MENOR que la entrada --\n");
    printf("   (probado con 1..%d muestras; 1 muestra = 1 fila del CSV = 1 valor por cada serie;\n", maxn);
    printf("    valores = muestras x series; 'ahorro' contra la propia entrada; 'vs JSON' contra el JSON de ese tamaño)\n");
    printf("\n%-16s %8s %7s %8s %9s %9s %9s %10s %14s\n",
           "combinacion", "muestras", "series", "valores", "entrada", "salida", "ahorro", "vs JSON", "estable desde");
    printf("-----------------------------------------------------------------------------------------------------\n");
    for (int c = 0; c < 4; c++) {
        int primero = 0, estable = 0;
        for (int n = 1; n <= maxn; n++) if (sale_menos(&ev[n], c)) { primero = n; break; }
        for (int n = maxn; n >= 1 && sale_menos(&ev[n], c); n--) estable = n;
        if (!primero) {
            printf("%-16s no comprime con ninguna de 1..%d muestras\n", nom[c], maxn);
            continue;
        }
        size_t in; long z;
        datos_comb(&ev[primero], c, &in, &z);
        printf("%-16s %8d %7d %8d %7zu B %7ld B %+8.1f%% %+9.1f%% ", nom[c], primero,
               ev[primero].series, ev[primero].valores, in, z,
               (1.0 - (double)z / (double)in) * 100.0,
               (1.0 - (double)z / (double)ev[primero].nj) * 100.0);
        if (estable) printf("%8d\n", estable);
        else         printf("%8s\n", "no");
    }
}
/* ---- TAMAÑO MINIMO EN BYTES / BITS ---- */

/* Prueba prefijos de 1..len bytes de la trama y busca, para cada combinacion:
 *  (a) el primer tamaño de entrada donde la salida es MENOR que la entrada
 *  (b) desde que tamaño ya no vuelve a agrandar nunca mas (hasta el tope) */

static void minimo_bytes(const Trama *t, const Ev *ev1)
{
    static const char *nom[4] = { "JSON + LZ78", "JSON + TAMP", "FORMATO + LZ78", "FORMATO + TAMP" };
    enum { TOPE = 4096 };
    size_t pri[4] = { 0 };
 
    printf("\n-- TAMAÑO MINIMO PARA COMPRIMIR (prefijos de la trama mas larga, byte a byte) --\n");
    printf("   (se toman los primeros l bytes de la trama larga, l = 1,2,3...; NO es una trama valida:\n");
    printf("    el corte puede caer en medio de un valor, y en el FORMATO los primeros bytes son los de UN solo campo)\n");
    printf("\n%-16s %-22s %9s %9s  %-22s\n",
           "combinacion", "empieza a comprimir", "salida", "ahorro", "comprime siempre desde");
    printf("--------------------------------------------------------------------------------------------\n");
    for (int c = 0; c < 4; c++) {
        const uint8_t *d = (const uint8_t *)(c < 2 ? t->json : t->bare);
        size_t len = c < 2 ? t->nj : t->nb, primero = 0, ultimo_mal = 0, cap;
        long zp = 0;
        uint8_t *z;
        char a[48], b[48];
 
        if (len > TOPE) len = TOPE;
        cap = len * 3 + 64;
        z = malloc(cap);
        if (!z) return;
        for (size_t l = 1; l <= len; l++) {
            long s = (c % 2 == 0) ? lz78_compress_mem(d, l, z, cap)
                                  : tamp_compress_mem(d, l, z, cap, W, L);
            int bien = s >= 0 && s < (long)l;
            if (bien && !primero) { primero = l; zp = s; }
            if (!bien) ultimo_mal = l;
        }
        free(z);
        if (!primero) {
            printf("%-16s no comprime con ningun prefijo de 1..%zu bytes\n", nom[c], len);
            continue;
        }
        pri[c] = primero;
        snprintf(a, sizeof a, "%zu B (%zu bits)", primero, primero * 8);
        if (ultimo_mal < len) snprintf(b, sizeof b, "%zu B (%zu bits)", ultimo_mal + 1, (ultimo_mal + 1) * 8);
        else                  snprintf(b, sizeof b, "no (hasta %zu B)", len);
        printf("%-16s %-22s %7ld B %+8.1f%%  %-22s\n", nom[c], a, zp,
               (1.0 - (double)zp / (double)primero) * 100.0, b);
    }
 
    printf("\n-- CONTENIDO EXACTO DE ESOS BYTES (primeros bytes de la trama larga, sin comprimir) --");
    for (int c = 0; c < 4; c++) {
        if (!pri[c]) continue;
        mostrar_prefijo(nom[c], (const uint8_t *)(c < 2 ? t->json : t->bare), pri[c],
                        c < 2 ? ev1->nj : ev1->nb, c < 2);
    }
}

/* ---- COMPOSICION: subconjuntos de columnas del CSV ---- */

#define MAXCOL 64

static int contar_filas(const uint8_t *d, size_t n)   /* filas de datos (sin encabezado) */
{
    int lineas = 0, util = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || d[i] == '\n') { lineas += util; util = 0; }
        else if (d[i] != '\r' && d[i] != ' ' && d[i] != '\t') util = 1;
    }
    return lineas > 0 ? lineas - 1 : 0;
}

static int partir(char *linea, char **campo)          /* corta por comas, en el lugar */
{
    int k = 0;
    campo[k++] = linea;
    for (char *p = linea; *p; p++)
        if (*p == ',' && k < MAXCOL) { *p = 0; campo[k++] = p + 1; }
    return k;
}

static void sin_comillas(char *s)
{
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') { s[l - 1] = 0; memmove(s, s + 1, l - 1); }
}

static int es_id(const char *s)
{
    return !strcasecmp(s, "deviceid") || !strcasecmp(s, "nodeid") ||
           !strcasecmp(s, "boardid")  || !strcasecmp(s, "board_id");
}

/* Escribe en dst un CSV con la columna de id + las columnas pedidas (en el
 * orden del encabezado original). Devuelve 0 ok, -1 error de E/S, o k>0 si la
 * columna cols[k-1] no existe en el CSV. */
static int csv_subconjunto(const uint8_t *d, size_t n, const char *dst, const char *const *cols)
{
    char *txt = malloc(n + 1), *campo[MAXCOL];
    int keep[MAXCOL] = { 0 }, ncol = 0, primera = 1, res = 0;
    if (!txt) return -1;
    memcpy(txt, d, n); txt[n] = 0;
    FILE *f = fopen(dst, "w");
    if (!f) { free(txt); return -1; }

    char *p = txt;
    while (*p) {
        char *fin = strchr(p, '\n');
        char *sig = fin ? fin + 1 : p + strlen(p);
        if (fin) *fin = 0;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r') p[--l] = 0;
        if (l) {
            int k = partir(p, campo);
            if (primera) {
                ncol = k;
                for (int i = 0; i < k; i++) {
                    sin_comillas(campo[i]);
                    keep[i] = es_id(campo[i]);
                    for (int j = 0; cols[j]; j++) if (!strcmp(campo[i], cols[j])) keep[i] = 1;
                }
                for (int j = 0; cols[j] && !res; j++) {
                    int hay = 0;
                    for (int i = 0; i < k; i++) if (!strcmp(campo[i], cols[j])) hay = 1;
                    if (!hay) res = j + 1;
                }
                if (res) break;
                primera = 0;
            }
            for (int i = 0, first = 1; i < k && i < ncol; i++)
                if (keep[i]) { fprintf(f, "%s%s", first ? "" : ",", campo[i]); first = 0; }
            fputc('\n', f);
        }
        p = sig;
    }
    fclose(f); free(txt);
    return res;
}

/* Composiciones a probar (nombres de columna del CSV; entre parentesis, ids de sensor). */
static const struct { const char *nombre; const char *cols[12]; } COMP[] = {
    { "bateria (9)",               { "battery", NULL } },
    { "ds18b20 (1)",               { "temperature", NULL } },
    { "sht3x (2)",                 { "temperature", "humidity", NULL } },
    { "sht3x+bateria (2,9)",       { "temperature", "humidity", "battery", NULL } },
    { "weather_station (3)",       { "wind_direction", "wind_gust", "wind_speed", "temperature",
                                     "humidity", "pressure", NULL } },
    { "weather_station+bat (3,9)", { "wind_direction", "wind_gust", "wind_speed", "temperature",
                                     "humidity", "pressure", "battery", NULL } },
    { "ws+sht3x_aux+bat (3,2,9)",  { "temperature", "temperature_aux", "humidity", "humidity_aux",
                                     "pressure", "wind_direction", "wind_gust", "wind_speed",
                                     "battery", NULL } },
};

/* ---- modo completo ---- */

static int modo_flare(const char *csv, int maxn, int comp_n, long off, int jmodo)
{
    char p1[1100], p2[1100];
    snprintf(p1, sizeof p1, "%s/csv_to_json.py", tools_dir);
    snprintf(p2, sizeof p2, "%s/encode_flaresense.py", tools_dir);
    if (access(p1, R_OK) || access(p2, R_OK)) {
        fprintf(stderr, "no encuentro csv_to_json.py y encode_flaresense.py en '%s' (usar --tools CARPETA)\n",
                tools_dir);
        return 2;
    }
     if (out_dir[0] && mkdir(out_dir, 0755) != 0 && errno != EEXIST) { perror(out_dir); return 2; }
    size_t n = 0;
    uint8_t *d = leer_todo(csv, &n);
    if (!d) return 1;
    long filas = contar_filas(d, n) - off;
    if (filas < 1) {
        fprintf(stderr, "el CSV no tiene filas de datos (con --offset %ld)\n", off);
        free(d); return 2;
    }
    if (maxn > filas) maxn = (int)filas;
    if (comp_n > filas) comp_n = (int)filas;

    Ev *ev = calloc((size_t)maxn + 1, sizeof *ev);
    if (!ev) { free(d); return 1; }
    int fallas = 0;

    printf("== FlareSense: %s (%ld filas de datos) ==\n", csv, filas);
    static const char *JNOM[3] = {
        "crudo (tal cual sale de csv_to_json.py --indent 0)",
        "compacto (el crudo sin espacios)",
        "minuta (sin fw/com_mode/rssi, con delta_t)" };
    printf("JSON %s | TAMP ventana %d, literal %d\n", JNOM[jmodo], W, L);
    printf("Cada celda: bytes y ahorro ('!' = fallo el round-trip). Contra que se mide:\n");
    printf("  FORMATO y JSON+algoritmo -> contra el JSON | FORMATO+algoritmo -> contra el FORMATO (ya comprimido)\n");

    /* barrido 1..maxn */
    Trama ej = { 0 };
    fprintf(stderr, "armando tramas de 1 a %d muestras...\n", maxn);
    for (int k = 1; k <= maxn; k++) {
        Trama t;
        if (armar_trama(csv, k, off, jmodo, &t)) { free(ev); free(d); return 1; }
        fallas += evaluar(&t, &ev[k]);
        { char pre[32]; snprintf(pre, sizeof pre, "n%03d", k); guardar_evidencia(pre, &t); }
        if (k == 1) ej = t; else liberar(&t);
    }

    if (ej.json && ej.bare)
            printf("\nEjemplo (1 muestra):\n  JSON    (%zu B): %s\n  FORMATO (%zu B): %s\n", ej.nj, ej.json, ej.nb, ej.bare);
    #if 0
      if (ej.json && ej.bare) {
          printf("\nQue contiene 1 muestra (1 fila del CSV):\n  FORMATO:\n");
          anotar_formato(ej.bare);
          printf("  JSON:\n");
          anotar_json(ej.json);
      }
    #endif
    liberar(&ej);

    printf("\n-- Barrido por cantidad de muestras --");
    encabezado_flare("muestras");
    for (int k = 1; k <= maxn; k++) {
        char et[32];
        if (!es_hito(k, maxn)) continue;
       snprintf(et, sizeof et, "%d muestra%s (%d valores)", k, k == 1 ? "" : "s", ev[k].valores);
        fila_flare(et, &ev[k]);
    }

    trama_minima(ev, maxn);

     Trama larga;
   if (!armar_trama(csv, maxn, off, jmodo, &larga)) { minimo_bytes(&larga, &ev[1]); liberar(&larga); }

    /* composicion de la trama */
    printf("\n-- COMPOSICION DE LA TRAMA (que sensores lleva), con 1 y %d muestras --", comp_n);
    encabezado_flare("composicion");
    char tmp[] = "/tmp/simple_flare_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        perror("mkstemp");
    } else {
        close(fd);
      struct { const char *nom; int nvalores; double fj, pj, fb, pb; } costo[16];
       int ncosto = 0;
        for (size_t c = 0; c < sizeof COMP / sizeof COMP[0]; c++) {
            int r = csv_subconjunto(d, n, tmp, COMP[c].cols);
            if (r < 0) { fprintf(stderr, "error armando CSV temporal\n"); fallas++; break; }
            if (r > 0) {
                printf("%-32s (se salta: el CSV no tiene la columna '%s')\n", COMP[c].nombre, COMP[c].cols[r - 1]);
                continue;
            }
            int ks[2] = { 1, comp_n }, nk = comp_n > 1 ? 2 : 1;
            for (int j = 0; j < nk; j++) {
               #if 0
            int ks[2] = { 1, comp_n }, nk = comp_n > 1 ? 2 : 1;
            Ev e1 = { 0 }, ek = { 0 };
            for (int j = 0; j < nk; j++) {
                Trama t; Ev e; char et[64];
                if (armar_trama(tmp, ks[j], off, jmodo, &t)) { fallas++; continue; }
                fallas += evaluar(&t, &e);
                snprintf(et, sizeof et, "%s [n=%d]", COMP[c].nombre, ks[j]);
                fila_flare(et, &e);
                { char pre[48]; snprintf(pre, sizeof pre, "comp%zu_n%03d", c, ks[j]); guardar_evidencia(pre, &t); }
                if (j == 0) e1 = e; else ek = e;
                liberar(&t);
            }
            if (nk == 2 && ncosto < 16) {         /* promedio por muestra = (tam(n) - tam(1)) / (n-1) */
                int nv = 0; while (COMP[c].cols[nv]) nv++;
                double dn = (double)(comp_n - 1);
                costo[ncosto].nom = COMP[c].nombre;
                costo[ncosto].nvalores = nv;
                costo[ncosto].pj = ((double)ek.nj - (double)e1.nj) / dn;
                costo[ncosto].fj = (double)e1.nj - costo[ncosto].pj;
                costo[ncosto].pb = ((double)ek.nb - (double)e1.nb) / dn;
                costo[ncosto].fb = (double)e1.nb - costo[ncosto].pb;
                ncosto++;
            }
        #endif
            }
        }
      #if 0
        printf("\n-- COSTO EN BYTES POR COMPOSICION (sin comprimir): fijo + por muestra --\n");
        printf("   (por muestra = promedio de (n=%d) - (n=1) sobre %d; fijo = lo que queda: encabezado/nodeId/etc.)\n",
               comp_n, comp_n - 1);
        printf("\n%-28s %8s | %-24s | %-24s\n", "composicion", "valores", "JSON (fijo + x muestra)", "FORMATO (fijo + x muestra)");
        printf("--------------------------------------------------------------------------------------\n");
        for (int i = 0; i < ncosto; i++)
            printf("%-28s %8d | %6.1f B + %6.1f B/muestra | %6.1f B + %6.1f B/muestra\n",
                   costo[i].nom, costo[i].nvalores, costo[i].fj, costo[i].pj, costo[i].fb, costo[i].pb);
#endif
        unlink(tmp);
    }
    if (out_dir[0]) printf("\nEvidencia guardada en '%s' (verificar tamaños con: ls -l %s)\n", out_dir, out_dir);
    printf("\nRESULTADO: %s\n", fallas ? "HAY FALLAS (ver celdas con '!' o ERROR)" : "todo OK");
    free(ev); free(d);
    return fallas ? 1 : 0;
}


static void uso(const char *p)
{
    fprintf(stderr,
        "uso: %s test [--size N]\n"
        "     %s ARCHIVO [--rows K] [--show S] [--no-header]\n"
        "     %s ARCHIVO.csv --flare [--tools DIR] [--out DIR] [--python CMD] [--max-n N]\n"
        "                    [--comp-n N] [--offset K] [--json crudo|compacto|minuta]\n"
        "     opciones generales: -w VENTANA(8..15) -l LITERAL\n", p, p, p);
}

/* ======================= main ============================================ */

int main(int argc, char **argv)
{
    const char *objetivo = NULL;
    int K = 1, mostrar = 10, con_header = 1;
    int flare = 0, max_n = 30, comp_n = 10, json_modo = 0;
    long offset = 0;
    size_t N = 1000;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int tiene_val = i + 1 < argc;
        if      (!strcmp(a, "--rows")      && tiene_val) K = atoi(argv[++i]);
        else if (!strcmp(a, "--show")      && tiene_val) mostrar = atoi(argv[++i]);
        else if (!strcmp(a, "--size")      && tiene_val) N = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "-w")          && tiene_val) W = atoi(argv[++i]);
        else if (!strcmp(a, "-l")          && tiene_val) L = atoi(argv[++i]);
        else if (!strcmp(a, "--no-header"))              con_header = 0;
        else if (!strcmp(a, "--flare"))                  flare = 1;
        else if (!strcmp(a, "--json")      && tiene_val) {
            const char *v = argv[++i];
            if      (!strcmp(v, "crudo"))    json_modo = 0;
            else if (!strcmp(v, "compacto")) json_modo = 1;
            else if (!strcmp(v, "minuta"))   json_modo = 2;
            else { fprintf(stderr, "--json: crudo | compacto | minuta\n"); return 2; }
        }
        else if (!strcmp(a, "--max-n")     && tiene_val) max_n = atoi(argv[++i]);
        else if (!strcmp(a, "--comp-n")    && tiene_val) comp_n = atoi(argv[++i]);
        else if (!strcmp(a, "--offset")    && tiene_val) offset = atol(argv[++i]);
        else if (!strcmp(a, "--tools")     && tiene_val) snprintf(tools_dir, sizeof tools_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--out")       && tiene_val) snprintf(out_dir, sizeof out_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--python")    && tiene_val) snprintf(python_cmd, sizeof python_cmd, "%s", argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { uso(argv[0]); return 0; }
        else if (a[0] != '-' && !objetivo)               objetivo = a;
        else { fprintf(stderr, "opcion no valida: %s\n", a); uso(argv[0]); return 2; }
    }
    if (!objetivo) { uso(argv[0]); return 2; }
    if (K < 1 || mostrar < 0 || N < 2 || W < 8 || W > 15) {
        fprintf(stderr, "valores fuera de rango (rows>=1, show>=0, size>=2, ventana 8..15)\n");
        return 2;
    }
    if (flare) {
        if (max_n < 1 || comp_n < 1 || offset < 0) {
            fprintf(stderr, "valores fuera de rango (max-n>=1, comp-n>=1, offset>=0)\n");
            return 2;
        }
        return modo_flare(objetivo, max_n, comp_n, offset, json_modo);
    }
    return strcmp(objetivo, "test") == 0 ? modo_test(N)
                                         : modo_archivo(objetivo, K, mostrar, con_header);
}
