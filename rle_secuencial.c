/*
 * ============================================================================
 *  rle_secuencial.c — Compresión RLE (Versión Secuencial)
 *
 *  Muestra información detallada de los segmentos de memoria:
 *    - PILA (Stack): variables locales, dirección, tamaño
 *    - CÓDIGO (Text): funciones, sus direcciones
 *    - DATOS (Data/Heap): variables globales, memoria dinámica
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEGMENTO DATA: Variables globales (inicializadas y no inicializadas)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Variables en segmento DATA (inicializadas) */
static int g_initialized_var = 42;
static const char *g_program_name = "RLE Secuencial";

/* Variables en segmento BSS (no inicializadas, se inicializan a 0) */
static int g_uninitialized_var;
static size_t g_total_runs;

/* ═══════════════════════════════════════════════════════════════════════════
 *  ESTRUCTURAS DE DATOS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *data;      /* Puntero a HEAP */
} Image;

typedef struct {
    uint8_t *data;      /* Puntero a HEAP */
    size_t size;
    size_t capacity;
} Buffer;

typedef struct {
    atomic_size_t pixels_processed;
    atomic_size_t compressed_bytes;
    size_t total_pixels;
    atomic_int done;
} Progress;

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECLARACIONES ADELANTADAS (para mostrar direcciones de código)
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]);
static void buffer_init(Buffer *buf, size_t cap);
static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n);
static void rle_compress(const uint8_t *pixels, size_t num_pixels, Buffer *out, Progress *prog);
static void generate_synthetic(Image *img, uint32_t w, uint32_t h);

/* ═══════════════════════════════════════════════════════════════════════════
 *  INFORMACIÓN DEL SISTEMA OPERATIVO
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t get_thread_id(void) {
#ifdef __APPLE__
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
#else
    return (uint64_t)pthread_self();
#endif
}

static void get_memory_info(size_t *rss, size_t *virtual_size) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        *rss = info.resident_size;
        *virtual_size = info.virtual_size;
    } else {
        *rss = 0;
        *virtual_size = 0;
    }
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long virt_pages, res_pages;
        fscanf(f, "%ld %ld", &virt_pages, &res_pages);
        fclose(f);
        long page_size = sysconf(_SC_PAGESIZE);
        *virtual_size = virt_pages * page_size;
        *rss = res_pages * page_size;
    }
#endif
}

static void get_process_cpu_times(double *user_s, double *sys_s) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *user_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    *sys_s  = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BUFFER DINÁMICO (HEAP)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void buffer_init(Buffer *buf, size_t cap) {
    buf->data = malloc(cap);  /* Asignación en HEAP */
    if (!buf->data) { perror("malloc"); exit(1); }
    buf->size = 0;
    buf->capacity = cap;
}

static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n) {
    while (buf->size + n > buf->capacity) {
        buf->capacity *= 2;
        buf->data = realloc(buf->data, buf->capacity);  /* Reasignación en HEAP */
        if (!buf->data) { perror("realloc"); exit(1); }
    }
    memcpy(buf->data + buf->size, bytes, n);
    buf->size += n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LECTURA PPM / GENERACIÓN SINTÉTICA
 * ═══════════════════════════════════════════════════════════════════════════ */

static int load_ppm(const char *path, Image *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[3];
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fclose(f); return -1;
    }

    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n'); }
        else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        else { ungetc(c, f); break; }
    }

    int w, h, maxval;
    if (fscanf(f, "%d %d %d", &w, &h, &maxval) != 3) { fclose(f); return -1; }
    fgetc(f);

    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    size_t pixel_bytes = (size_t)w * h * 3;
    img->data = malloc(pixel_bytes);  /* Asignación en HEAP */
    if (!img->data) { fclose(f); return -1; }

    if (fread(img->data, 1, pixel_bytes, f) != pixel_bytes) {
        free(img->data); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

static void generate_synthetic(Image *img, uint32_t w, uint32_t h) {
    img->width = w;
    img->height = h;
    size_t pixel_bytes = (size_t)w * h * 3;
    img->data = malloc(pixel_bytes);  /* Asignación en HEAP */
    if (!img->data) { perror("malloc"); exit(1); }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t band = y / 8;
        uint8_t r = (uint8_t)(band * 37 % 256);
        uint8_t g = (uint8_t)(band * 59 % 256);
        uint8_t b = (uint8_t)(band * 91 % 256);
        for (uint32_t x = 0; x < w; x++) {
            size_t idx = ((size_t)y * w + x) * 3;
            img->data[idx]     = r;
            img->data[idx + 1] = g;
            img->data[idx + 2] = b;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  COMPRESIÓN RLE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rle_compress(const uint8_t *pixels, size_t num_pixels,
                         Buffer *out, Progress *prog) {
    size_t i = 0;
    while (i < num_pixels) {
        uint8_t r = pixels[i * 3];
        uint8_t g = pixels[i * 3 + 1];
        uint8_t b = pixels[i * 3 + 2];
        uint8_t count = 1;

        while (i + count < num_pixels && count < 255 &&
               pixels[(i + count) * 3]     == r &&
               pixels[(i + count) * 3 + 1] == g &&
               pixels[(i + count) * 3 + 2] == b) {
            count++;
        }

        uint8_t run[4] = { count, r, g, b };
        buffer_push(out, run, 4);
        i += count;
        g_total_runs++;  /* Variable global en BSS */

        atomic_store(&prog->pixels_processed, i);
        atomic_store(&prog->compressed_bytes, out->size);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  VISUALIZACIÓN DE SEGMENTOS DE MEMORIA
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_memory_segments(Image *img, Buffer *compressed,
                                   void *stack_top, void *stack_bottom) {
    const char *CYAN = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN = "\033[32m";
    const char *RED = "\033[31m";
    const char *MAGENTA = "\033[35m";
    const char *WHITE = "\033[1;37m";
    const char *RESET = "\033[0m";

    size_t rss, virt;
    get_memory_info(&rss, &virt);

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s     %s██████╗ ██╗██╗      █████╗      SEGMENTOS DE MEMORIA DEL PROCESO%s                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s██╔══██╗██║██║     ██╔══██╗     (Pila, Código, Datos)%s                           %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s██████╔╝██║██║     ███████║%s                                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s██╔═══╝ ██║██║     ██╔══██║     MODO: SECUENCIAL (1 hilo)%s                       %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s██║     ██║███████╗██║  ██║%s                                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s╚═╝     ╚═╝╚══════╝╚═╝  ╚═╝%s                                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: PILA (STACK)
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: PILA (STACK)%s                                                      %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Descripción: Almacena variables locales, parámetros de funciones,            %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               direcciones de retorno. Crece hacia direcciones BAJAS.          %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sRW- (lectura/escritura, no ejecutable)%s                             %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Tamaño máximo: %s8 MB%s (por defecto en macOS)                                   %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─────────────────────────────────────────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %sVariable                Dirección           Tamaño    Valor%s          │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  ──────────────────────  ──────────────────  ────────  ──────────────  │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  stack_top (local)      %s0x%014lx%s    8 bytes   (tope pila)     │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)stack_top, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  stack_bottom (local)   %s0x%014lx%s    8 bytes   (base pila)     │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)stack_bottom, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  img (struct local)     %s0x%014lx%s   16 bytes   Image struct    │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)img, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  compressed (local)     %s0x%014lx%s   24 bytes   Buffer struct   │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)compressed, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │                                                                         │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Tamaño usado en stack: %s~%lu bytes%s                                      │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, GREEN, (unsigned long)((char*)stack_top - (char*)stack_bottom), RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: CÓDIGO (TEXT)
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: CÓDIGO (TEXT)%s                                                     %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Descripción: Contiene las instrucciones de máquina del programa compilado.   %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               Es de SOLO LECTURA para evitar modificaciones accidentales.     %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sR-X (lectura/ejecución, no escritura)%s                              %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, GREEN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─────────────────────────────────────────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %sFunción                 Dirección           Descripción%s              │ %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  ──────────────────────  ──────────────────  ─────────────────────────  │ %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  main()                 %s0x%014lx%s  Punto de entrada           │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)main, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  rle_compress()         %s0x%014lx%s  Algoritmo RLE              │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)rle_compress, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  buffer_init()          %s0x%014lx%s  Inicializar buffer         │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)buffer_init, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  buffer_push()          %s0x%014lx%s  Agregar a buffer           │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)buffer_push, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  generate_synthetic()   %s0x%014lx%s  Generar imagen             │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)generate_synthetic, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: DATOS (DATA + BSS + HEAP)
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: DATOS (DATA + BSS + HEAP)%s                                         %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[DATA]%s Variables globales inicializadas                                      %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[BSS]%s  Variables globales no inicializadas (se inicializan a 0)              %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[HEAP]%s Memoria dinámica asignada con malloc()                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sRW- (lectura/escritura, no ejecutable)%s                             %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─ DATA (variables inicializadas) ────────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_initialized_var      %s0x%014lx%s    4 bytes   valor: %d       │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_initialized_var, RESET, g_initialized_var, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_program_name         %s0x%014lx%s    8 bytes   \"%s\"   │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_program_name, RESET, g_program_name, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─ BSS (variables no inicializadas) ─────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_uninitialized_var    %s0x%014lx%s    4 bytes   valor: %d        │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_uninitialized_var, RESET, g_uninitialized_var, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_total_runs           %s0x%014lx%s    8 bytes   valor: %zu     │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_total_runs, RESET, g_total_runs, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─ HEAP (memoria dinámica - malloc) ─────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  img.data               %s0x%014lx%s  %10zu bytes (imagen)    │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)img->data, RESET,
           (size_t)img->width * img->height * 3, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  compressed.data        %s0x%014lx%s  %10zu bytes (buffer)    │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)compressed->data, RESET,
           compressed->capacity, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │                                                                         │ %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Total HEAP usado:      %s%zu bytes (%.2f MB)%s                            │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, GREEN,
           (size_t)img->width * img->height * 3 + compressed->capacity,
           ((size_t)img->width * img->height * 3 + compressed->capacity) / (1024.0 * 1024.0),
           RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);

    /* Resumen de memoria */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ RESUMEN DE MEMORIA DEL PROCESO ───────────────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  Memoria Física (RSS):    %s%10.2f MB%s                                         │  %s║%s\n",
           CYAN, RESET, GREEN, rss / (1024.0 * 1024.0), RESET, CYAN, RESET);
    printf("%s║%s  │  PID:                     %s%10d%s                                              │  %s║%s\n",
           CYAN, RESET, GREEN, getpid(), RESET, CYAN, RESET);
    printf("%s║%s  │  TID (hilo principal):    %s0x%-8lx%s                                            │  %s║%s\n",
           CYAN, RESET, GREEN, (unsigned long)get_thread_id(), RESET, CYAN, RESET);
    printf("%s║%s  │  Hilos de trabajo:        %s         1%s  (secuencial)                             │  %s║%s\n",
           CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │  Cores disponibles:       %s%10d%s                                              │  %s║%s\n",
           CYAN, RESET, GREEN, (int)sysconf(_SC_NPROCESSORS_ONLN), RESET, CYAN, RESET);
    printf("%s║%s  %s└───────────────────────────────────────────────────────────────────────────────┘%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

static void print_execution_results(double elapsed, double user_t, double sys_t,
                                     size_t compressed_size, size_t raw_size) {
    const char *CYAN = "\033[36m";
    const char *GREEN = "\033[32m";
    const char *YELLOW = "\033[33m";
    const char *WHITE = "\033[1;37m";
    const char *RESET = "\033[0m";

    double cpu_total = user_t + sys_t;
    double cpu_pct = elapsed > 0 ? (cpu_total / elapsed) * 100.0 : 0;
    double ratio = (1.0 - (double)compressed_size / raw_size) * 100.0;
    double throughput = elapsed > 0 ? (raw_size / (1024.0 * 1024.0)) / elapsed : 0;

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s                       %s*** RESULTADOS DE EJECUCIÓN ***%s                               %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sTiempo wall (real):%s         %s%12.6f%s segundos                                     %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, elapsed, RESET, CYAN, RESET);
    printf("%s║%s  %sTiempo CPU (usuario):%s       %s%12.6f%s segundos                                     %s║%s\n",
           CYAN, RESET, WHITE, RESET, YELLOW, user_t, RESET, CYAN, RESET);
    printf("%s║%s  %sTiempo CPU (sistema):%s       %s%12.6f%s segundos                                     %s║%s\n",
           CYAN, RESET, WHITE, RESET, YELLOW, sys_t, RESET, CYAN, RESET);
    printf("%s║%s  %sTiempo CPU (total):%s         %s%12.6f%s segundos                                     %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, cpu_total, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sUso de CPU:%s                  %s%12.1f%%%s                                             %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, cpu_pct, RESET, CYAN, RESET);
    printf("%s║%s  %sThroughput:%s                  %s%12.1f%s MB/s                                         %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, throughput, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sTamaño original:%s             %s%12zu%s bytes                                        %s║%s\n",
           CYAN, RESET, WHITE, RESET, YELLOW, raw_size, RESET, CYAN, RESET);
    printf("%s║%s  %sTamaño comprimido:%s           %s%12zu%s bytes                                        %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, compressed_size, RESET, CYAN, RESET);
    printf("%s║%s  %sRatio de compresión:%s         %s%12.1f%%%s                                            %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, ratio, RESET, CYAN, RESET);
    printf("%s║%s  %sRuns generados:%s              %s%12zu%s                                              %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, g_total_runs, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FUNCIÓN PRINCIPAL
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Variables en STACK */
    Image img;
    Buffer compressed;
    Progress prog;
    int stack_marker_top = 0;    /* Para medir tope de pila */

    /* Cargar o generar imagen */
    if (argc >= 2) {
        if (load_ppm(argv[1], &img) != 0) {
            fprintf(stderr, "Error: no se pudo leer '%s' como PPM P6\n", argv[1]);
            return 1;
        }
    } else {
        generate_synthetic(&img, 4096, 4096);
    }

    size_t total_pixels = (size_t)img.width * img.height;
    size_t raw_size = total_pixels * 3;

    /* Inicializar buffer (asigna en HEAP) */
    buffer_init(&compressed, raw_size / 2);

    /* Inicializar progreso */
    atomic_init(&prog.pixels_processed, 0);
    atomic_init(&prog.compressed_bytes, 0);
    prog.total_pixels = total_pixels;
    atomic_init(&prog.done, 0);

    int stack_marker_bottom = 0;  /* Para medir base de pila */

    /* Mostrar segmentos de memoria ANTES de ejecutar */
    print_memory_segments(&img, &compressed, &stack_marker_top, &stack_marker_bottom);

    printf("\n\033[33m  Ejecutando compresión RLE (1 hilo)...\033[0m\n");

    /* Medir tiempo */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* COMPRESIÓN */
    rle_compress(img.data, total_pixels, &compressed, &prog);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* Obtener tiempos CPU */
    double user_t, sys_t;
    get_process_cpu_times(&user_t, &sys_t);

    /* Mostrar resultados */
    print_execution_results(elapsed, user_t, sys_t, compressed.size + 8, raw_size);

    /* Escribir archivo de salida */
    char outpath[512];
    if (argc >= 2)
        snprintf(outpath, sizeof(outpath), "%s.rle", argv[1]);
    else
        snprintf(outpath, sizeof(outpath), "output_secuencial.rle");

    FILE *fout = fopen(outpath, "wb");
    if (fout) {
        uint32_t header[2] = { img.width, img.height };
        fwrite(header, sizeof(uint32_t), 2, fout);
        fwrite(compressed.data, 1, compressed.size, fout);
        fclose(fout);
        printf("\n  Archivo guardado: %s\n\n", outpath);
    }

    /* Liberar memoria del HEAP */
    free(img.data);
    free(compressed.data);
    return 0;
}
