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
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <malloc/malloc.h>  /* macOS: malloc_size() */

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#endif

/* stb_image: carga PNG, JPG, BMP, GIF, TGA, PSD, HDR, PIC */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
 *  SEGUIMIENTO DE FASES DEL PROGRAMA
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum { PHASE_INIT=0, PHASE_COMPRESS, PHASE_DECOMPRESS, PHASE_OUTPUT, PHASE_CLEANUP, PHASE_COUNT } ProgramPhase;
static const char *g_phase_names[] = {"Inicializacion", "Compresion", "Descompresion", "Salida", "Limpieza"};
static ProgramPhase g_current_phase = PHASE_INIT;

/* Seguimiento de syscalls (max 32 entradas) */
typedef struct { const char *name; const char *real_syscall; const char *purpose; int count_by_phase[PHASE_COUNT]; int total; } SyscallEntry;
static SyscallEntry g_syscall_table[32];
static int g_num_tracked_syscalls = 0;

/* Seguimiento de asignaciones en heap (max 32 entradas) */
typedef struct { void *addr; size_t requested_size; size_t actual_size; const char *label; int freed; } HeapAllocation;
static HeapAllocation g_heap_log[32];
static int g_num_heap_entries = 0;

/* Seguimiento de señales (max 8 entradas) */
typedef struct { int signum; const char *name; const char *description; volatile sig_atomic_t received_count; } SignalEntry;
static SignalEntry g_signal_table[8];
static int g_num_signals = 0;

/* Baseline de cambios de contexto */
static long g_baseline_vcsw = 0, g_baseline_ivcsw = 0;
static long g_baseline_minflt = 0, g_baseline_majflt = 0;

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

/* Muestreo del PC (Program Counter) para el hilo secuencial */
#define MAX_PC_SAMPLES 256

typedef struct {
    double    timestamp_ms;
    uintptr_t pc_addr;
    size_t    pixels_at;
} PCSample;

static PCSample g_pc_samples[MAX_PC_SAMPLES];
static int g_num_pc_samples = 0;
static struct timespec g_t0_compress; /* Tiempo base de la compresión */

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECLARACIONES ADELANTADAS (para mostrar direcciones de código)
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]);
static void buffer_init(Buffer *buf, size_t cap);
static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n);
static void rle_compress(const uint8_t *pixels, size_t num_pixels, Buffer *out, Progress *prog);
static void generate_synthetic(Image *img, uint32_t w, uint32_t h);
static int load_image(const char *path, Image *img);
static uint8_t *rle_decompress(const uint8_t *rle_data, size_t rle_size,
                                size_t expected_pixels);
static void save_bmp(const char *path, const uint8_t *pixels, uint32_t w, uint32_t h);

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
 *  FUNCIONES AUXILIARES DE SEGUIMIENTO (Tracking Helpers)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void track_syscall(const char *name, const char *real_syscall, const char *purpose) {
    for (int i = 0; i < g_num_tracked_syscalls; i++) {
        if (strcmp(g_syscall_table[i].name, name) == 0 && strcmp(g_syscall_table[i].purpose, purpose) == 0) {
            g_syscall_table[i].count_by_phase[g_current_phase]++;
            g_syscall_table[i].total++;
            return;
        }
    }
    if (g_num_tracked_syscalls < 32) {
        int idx = g_num_tracked_syscalls++;
        g_syscall_table[idx].name = name;
        g_syscall_table[idx].real_syscall = real_syscall;
        g_syscall_table[idx].purpose = purpose;
        memset(g_syscall_table[idx].count_by_phase, 0, sizeof(int)*PHASE_COUNT);
        g_syscall_table[idx].count_by_phase[g_current_phase] = 1;
        g_syscall_table[idx].total = 1;
    }
}

static void track_heap_alloc(void *addr, size_t requested, const char *label) {
    if (g_num_heap_entries < 32 && addr) {
        HeapAllocation *e = &g_heap_log[g_num_heap_entries++];
        e->addr = addr; e->requested_size = requested;
        e->actual_size = malloc_size(addr);
        e->label = label; e->freed = 0;
    }
}

static void track_heap_free(void *addr) {
    for (int i = 0; i < g_num_heap_entries; i++)
        if (g_heap_log[i].addr == addr && !g_heap_log[i].freed) { g_heap_log[i].freed = 1; return; }
}

static void demo_signal_handler(int sig) {
    for (int i = 0; i < g_num_signals; i++)
        if (g_signal_table[i].signum == sig) { g_signal_table[i].received_count++; return; }
}

static void register_signal(int signum, const char *name, const char *desc) {
    if (g_num_signals < 8) {
        g_signal_table[g_num_signals].signum = signum;
        g_signal_table[g_num_signals].name = name;
        g_signal_table[g_num_signals].description = desc;
        g_signal_table[g_num_signals].received_count = 0;
        struct sigaction sa;
        sa.sa_handler = demo_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(signum, &sa, NULL);
        g_num_signals++;
    }
}

static void setup_signal_handlers(void) {
    register_signal(SIGSEGV, "SIGSEGV", "Acceso a memoria invalida");
    register_signal(SIGFPE,  "SIGFPE",  "Error aritmetico (div/0)");
    register_signal(SIGBUS,  "SIGBUS",  "Error de bus (alignment)");
    register_signal(SIGABRT, "SIGABRT", "Abortar (assert/abort)");
    register_signal(SIGINT,  "SIGINT",  "Interrupcion (Ctrl+C)");
    register_signal(SIGTERM, "SIGTERM", "Terminacion solicitada");
    track_syscall("sigaction", "rt_sigaction", "Registrar manejador de senial");
}

static void capture_baseline_ctx(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    g_baseline_vcsw = ru.ru_nvcsw; g_baseline_ivcsw = ru.ru_nivcsw;
    g_baseline_minflt = ru.ru_minflt; g_baseline_majflt = ru.ru_majflt;
    track_syscall("getrusage", "getrusage", "Leer estadisticas del proceso (baseline)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BUFFER DINÁMICO (HEAP)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void buffer_init(Buffer *buf, size_t cap) {
    buf->data = malloc(cap);  /* Asignación en HEAP */
    if (!buf->data) { perror("malloc"); exit(1); }
    buf->size = 0;
    buf->capacity = cap;
    track_syscall("malloc", "mmap/brk", "Asignar buffer de compresion");
    track_heap_alloc(buf->data, cap, "Buffer compresion");
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
 *  CARGA DE IMAGEN (PNG, JPG, BMP, GIF, TGA, PSD, HDR, PIC via stb_image)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int load_image(const char *path, Image *img) {
    int w, h, channels;
    /* stb_image carga en RGB (3 canales) para comparación justa con versión paralela */
    uint8_t *pixels = stbi_load(path, &w, &h, &channels, 3);
    if (!pixels) {
        fprintf(stderr, "  Error cargando '%s': %s\n", path, stbi_failure_reason());
        return -1;
    }
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->data = pixels;  /* stb_image usa malloc internamente → datos en HEAP */
    track_syscall("stbi_load", "mmap/read", "Cargar imagen desde disco");
    track_heap_alloc(img->data, (size_t)w * h * 3, "Imagen RGB");
    printf("\n  \033[32mImagen cargada:\033[0m %s\n", path);
    printf("    Dimensiones: %d x %d px (%d canales originales → RGB)\n", w, h, channels);
    printf("    Tamaño datos: %zu bytes (%.2f MB)\n\n",
           (size_t)w * h * 3, (size_t)w * h * 3 / (1024.0 * 1024.0));
    return 0;
}

static void generate_synthetic(Image *img, uint32_t w, uint32_t h) {
    img->width = w;
    img->height = h;
    size_t pixel_bytes = (size_t)w * h * 3;
    img->data = malloc(pixel_bytes);
    if (!img->data) { perror("malloc"); exit(1); }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t band = y / 8;
        uint8_t gray = (uint8_t)(band * 17 % 256);
        for (uint32_t x = 0; x < w; x++) {
            size_t idx = ((size_t)y * w + x) * 3;
            img->data[idx + 0] = gray;
            img->data[idx + 1] = gray;
            img->data[idx + 2] = gray;
        }
}


}

/* ═══════════════════════════════════════════════════════════════════════════
 *  COMPRESIÓN RLE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rle_compress(const uint8_t *pixels, size_t num_pixels,
                         Buffer *out, Progress *prog) {
    size_t i = 0;
    size_t sample_interval = num_pixels / (MAX_PC_SAMPLES - 2);
    if (sample_interval < 1) sample_interval = 1;
    size_t next_sample = 0;

    /* Muestra PC inicial */
    if (g_num_pc_samples < MAX_PC_SAMPLES) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        g_pc_samples[g_num_pc_samples++] = (PCSample){
            .timestamp_ms = (now.tv_sec - g_t0_compress.tv_sec) * 1000.0 +
                            (now.tv_nsec - g_t0_compress.tv_nsec) / 1e6,
            .pc_addr = (uintptr_t)rle_compress,
            .pixels_at = 0
        };
    }

    while (i < num_pixels) {
        uint8_t gray = pixels[i];
        uint8_t count = 1;

        while (i + count < num_pixels && count < 255 &&
               pixels[i + count] == gray) {
            count++;
        }

        uint8_t run[2] = { count, gray };
        buffer_push(out, run, 2);
        i += count;
        g_total_runs++;

        atomic_store(&prog->pixels_processed, i);
        atomic_store(&prog->compressed_bytes, out->size);

        /* Muestrear PC periódicamente */
        if (i >= next_sample && g_num_pc_samples < MAX_PC_SAMPLES) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            g_pc_samples[g_num_pc_samples++] = (PCSample){
                .timestamp_ms = (now.tv_sec - g_t0_compress.tv_sec) * 1000.0 +
                                (now.tv_nsec - g_t0_compress.tv_nsec) / 1e6,
                .pc_addr = (uintptr_t)rle_compress + (i & 0xFFF),
                .pixels_at = i
            };
            next_sample += sample_interval;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DESCOMPRESIÓN RLE → PÍXELES RGB
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t *rle_decompress(const uint8_t *rle_data, size_t rle_size,
                                size_t expected_pixels) {
    uint8_t *pixels = (uint8_t *)malloc(expected_pixels);
    if (!pixels) { perror("malloc decompress"); return NULL; }

    size_t px = 0;
    size_t i = 0;
    while (i + 1 < rle_size && px < expected_pixels) {
        uint8_t count = rle_data[i];
        uint8_t gray = rle_data[i + 1];
        for (uint8_t j = 0; j < count && px < expected_pixels; j++) {
            pixels[px] = gray;
            px++;
        }
        i += 2;
    }
    return pixels;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  GUARDAR IMAGEN BMP (sin dependencias externas)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void save_bmp(const char *path, const uint8_t *pixels,
                      uint32_t w, uint32_t h) {
    uint32_t row_stride = (w * 3 + 3) & ~3u;
    uint32_t pixel_data_size = row_stride * h;
    uint32_t file_size = 14 + 40 + pixel_data_size;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen bmp"); return; }
    track_syscall("fopen", "open", "Abrir archivo BMP para escritura");

    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = file_size & 0xFF;
    fh[3] = (file_size >> 8) & 0xFF;
    fh[4] = (file_size >> 16) & 0xFF;
    fh[5] = (file_size >> 24) & 0xFF;
    uint32_t offset = 14 + 40;
    fh[10] = offset & 0xFF;
    fh[11] = (offset >> 8) & 0xFF;
    fwrite(fh, 1, 14, f);

    uint8_t ih[40] = {0};
    ih[0] = 40;
    ih[4] = w & 0xFF; ih[5] = (w >> 8) & 0xFF;
    ih[6] = (w >> 16) & 0xFF; ih[7] = (w >> 24) & 0xFF;
    ih[8] = h & 0xFF; ih[9] = (h >> 8) & 0xFF;
    ih[10] = (h >> 16) & 0xFF; ih[11] = (h >> 24) & 0xFF;
    ih[12] = 1;
    ih[14] = 24;
    ih[20] = pixel_data_size & 0xFF;
    ih[21] = (pixel_data_size >> 8) & 0xFF;
    ih[22] = (pixel_data_size >> 16) & 0xFF;
    ih[23] = (pixel_data_size >> 24) & 0xFF;
    fwrite(ih, 1, 40, f);

    uint8_t *row = (uint8_t *)calloc(row_stride, 1);
    if (!row) { fclose(f); return; }
    for (int y = (int)h - 1; y >= 0; y--) {
        memset(row, 0, row_stride);
        for (uint32_t x = 0; x < w; x++) {
            size_t src = ((size_t)y * w + x) * 3;
            row[x * 3 + 0] = pixels[src + 2];  /* B */
            row[x * 3 + 1] = pixels[src + 1];  /* G */
            row[x * 3 + 2] = pixels[src + 0];  /* R */
        }
        fwrite(row, 1, row_stride, f);
    }
    free(row);
    track_syscall("fwrite", "write", "Escribir datos BMP a disco");
    fclose(f);
    track_syscall("fclose", "close", "Cerrar archivo BMP");
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
    printf("%s║%s  %s▓%s  │  img.data               %s0x%014lx%s  %10zu bytes (RGB)       │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)img->data, RESET,
           (size_t)img->width * img->height * 3, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  compressed.data        %s0x%014lx%s  %10zu bytes (buffer)    │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)compressed->data, RESET,
           compressed->capacity, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │                                                                         │ %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Total HEAP usado:      %s%zu bytes (%.2f KB)%s                            │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, GREEN,
           (size_t)img->width * img->height * 3 + compressed->capacity,
           ((size_t)img->width * img->height * 3 + compressed->capacity) / 1024.0,
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
 *  1. VISUALIZACIÓN DE LLAMADAS AL SISTEMA (SYSCALLS)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_syscall_table(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *RED    = "\033[31m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sLLAMADAS AL SISTEMA OPERATIVO (System Calls)%s                              %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ Que es una syscall? ──────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ Una llamada al sistema (syscall) es el mecanismo por el cual un programa en    │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ modo usuario solicita un servicio al kernel del SO. Se ejecuta una instruccion │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ TRAP (int 0x80 / syscall / svc) que cambia de %sUser Mode%s a %sKernel Mode%s.         │%s║%s\n", CYAN, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │ El kernel ejecuta la operacion privilegiada y retorna con IRET/ERET.           │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla de syscalls */
    printf("%s║%s  %s┌──────────────┬────────────────┬─────────────────┬───────┬──────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %sFuncion C%s     │ %sSyscall real%s    │ %sFase%s              │%sConteo%s│ %sProposito%s             │%s║%s\n",
           CYAN, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s├──────────────┼────────────────┼─────────────────┼───────┼──────────────────────┤%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);

    int total_syscalls = 0;
    for (int i = 0; i < g_num_tracked_syscalls; i++) {
        /* Determinar la fase principal */
        int max_phase = 0;
        for (int p = 1; p < PHASE_COUNT; p++)
            if (g_syscall_table[i].count_by_phase[p] > g_syscall_table[i].count_by_phase[max_phase])
                max_phase = p;

        printf("%s║%s  │ %s%-12s%s │ %-14s │ %-15s │  %s%3d%s  │ %-20s │%s║%s\n",
               CYAN, RESET,
               GREEN, g_syscall_table[i].name, RESET,
               g_syscall_table[i].real_syscall,
               g_phase_names[max_phase],
               MAGENTA, g_syscall_table[i].total, RESET,
               g_syscall_table[i].purpose,
               CYAN, RESET);
        total_syscalls += g_syscall_table[i].total;
    }
    printf("%s║%s  %s└──────────────┴────────────────┴─────────────────┴───────┴──────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sTotal de llamadas al sistema registradas: %s%d%s                                       %s║%s\n",
           CYAN, RESET, WHITE, GREEN, total_syscalls, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Diagrama ASCII de transicion */
    printf("%s║%s  %s┌─ Ejemplo: malloc() → mmap ──────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │   %sPROGRAMA%s ──malloc()──> %slibc%s ──mmap()──> %sTRAP%s ──> %sKERNEL%s ──> %sIRET%s ──> %sPROGRAMA%s │%s║%s\n",
           CYAN, RESET, GREEN, RESET, YELLOW, RESET, RED, RESET, MAGENTA, RESET, RED, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │  %s(user mode)%s                             %s(switch)%s   %s(kernel)%s  %s(return)%s %s(user mode)%s│%s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, MAGENTA, RESET, RED, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  2. VISUALIZACIÓN DEL HEAP (Memoria Dinámica)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_heap_visualization(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *RED    = "\033[31m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sVISUALIZACION DEL HEAP (Memoria Dinamica)%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ Que es el Heap? ─────────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ El heap es la region de memoria para asignacion dinamica (malloc/free).        │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ Crece hacia direcciones ALTAS. El kernel asigna paginas via brk() o mmap().   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ En macOS, el allocator usa un sistema de %s\"magazines\"%s (libmalloc) que agrupa    │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │ bloques por tamanio para reducir fragmentacion. Bloques grandes (>64KB) usan  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ mmap() directamente, creando regiones independientes en el espacio virtual.   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla de asignaciones */
    printf("%s║%s  %s┌────┬──────────────────┬──────────────────┬────────────┬────────────┬────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %s#%s  │ %sEtiqueta%s         │ %sDireccion%s        │ %sSolicitado%s │ %sReal(msize)%s│%sEstado%s │  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s├────┼──────────────────┼──────────────────┼────────────┼────────────┼────────┤%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);

    size_t total_requested = 0, total_actual = 0;
    int active_count = 0, freed_count = 0;

    for (int i = 0; i < g_num_heap_entries; i++) {
        HeapAllocation *e = &g_heap_log[i];
        total_requested += e->requested_size;
        total_actual += e->actual_size;
        if (e->freed) freed_count++; else active_count++;

        printf("%s║%s  │ %s%2d%s │ %-16s │ %s0x%012lx%s   │ %s%8zu B%s │ %s%8zu B%s │  %s%s%s   │  %s║%s\n",
               CYAN, RESET,
               MAGENTA, i + 1, RESET,
               e->label,
               MAGENTA, (unsigned long)e->addr, RESET,
               GREEN, e->requested_size, RESET,
               YELLOW, e->actual_size, RESET,
               e->freed ? RED : GREEN,
               e->freed ? "LIBRE" : "ACTIV",
               RESET,
               CYAN, RESET);
    }
    printf("%s║%s  %s└────┴──────────────────┴──────────────────┴────────────┴────────────┴────────┘%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Estadisticas */
    double frag_pct = total_requested > 0 ? ((double)(total_actual - total_requested) / total_actual) * 100.0 : 0;
    printf("%s║%s  %s┌─ Estadisticas del Heap ────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  Total solicitado:         %s%10zu bytes%s  (%s%.2f MB%s)                         │%s║%s\n",
           CYAN, RESET, GREEN, total_requested, RESET, GREEN, total_requested / (1024.0 * 1024.0), RESET, CYAN, RESET);
    printf("%s║%s  │  Total real (malloc_size): %s%10zu bytes%s  (%s%.2f MB%s)                         │%s║%s\n",
           CYAN, RESET, YELLOW, total_actual, RESET, YELLOW, total_actual / (1024.0 * 1024.0), RESET, CYAN, RESET);
    printf("%s║%s  │  Fragmentacion interna:    %s%10.1f %%%s                                          │%s║%s\n",
           CYAN, RESET, RED, frag_pct, RESET, CYAN, RESET);
    printf("%s║%s  │  Bloques activos / libres: %s%d activos%s / %s%d liberados%s                              │%s║%s\n",
           CYAN, RESET, GREEN, active_count, RESET, RED, freed_count, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sNota:%s En macOS, el magazine allocator de libmalloc agrupa bloques pequenios en     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  regiones (\"magazines\"). Bloques >64KB se asignan directamente via mmap().          %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  malloc_size() retorna el tamanio real del bloque incluyendo overhead de alignment.  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  3. VISUALIZACIÓN DE VARIABLES GLOBALES - Segmentos DATA y BSS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_global_variables(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sVARIABLES GLOBALES - Segmentos DATA y BSS%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ DATA vs BSS ─────────────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %sDATA:%s Contiene variables globales/static con valor inicial explicito.         │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │       Se almacenan en el ejecutable en disco (ocupan espacio en el binario).   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %sBSS:%s  Contiene variables globales/static inicializadas a cero.                │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │       NO ocupan espacio en el binario; el kernel las inicializa a 0 al cargar. │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │       Esto reduce significativamente el tamanio del ejecutable.                │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla DATA */
    printf("%s║%s  %s┌─ Segmento DATA (variables inicializadas) ─────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │  %-24s %-18s %-8s %-20s │%s║%s\n",
           CYAN, RESET, "Variable", "Direccion", "Bytes", "Valor", CYAN, RESET);
    printf("%s║%s  │  ──────────────────────── ────────────────── ──────── ──────────────────── │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  g_initialized_var        %s0x%014lx%s   4       valor: %s%d%s               │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)&g_initialized_var, RESET, GREEN, g_initialized_var, RESET, CYAN, RESET);
    printf("%s║%s  │  g_program_name           %s0x%014lx%s   8       \"%s%s%s\"       │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)&g_program_name, RESET, GREEN, g_program_name, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla BSS */
    printf("%s║%s  %s┌─ Segmento BSS (variables zero-initialized) ──────────────────────────────────┐%s%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │  %-24s %-18s %-8s %-20s │%s║%s\n",
           CYAN, RESET, "Variable", "Direccion", "Bytes", "Contenido", CYAN, RESET);
    printf("%s║%s  │  ──────────────────────── ────────────────── ──────── ──────────────────── │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  g_uninitialized_var      %s0x%014lx%s   4       valor: %s%d%s               │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)&g_uninitialized_var, RESET, GREEN, g_uninitialized_var, RESET, CYAN, RESET);
    printf("%s║%s  │  g_total_runs             %s0x%014lx%s   8       valor: %s%zu%s             │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)&g_total_runs, RESET, GREEN, g_total_runs, RESET, CYAN, RESET);
    printf("%s║%s  │  g_pc_samples[%d]          %s0x%014lx%s   %5zu   array PCSample          │%s║%s\n",
           CYAN, RESET, MAX_PC_SAMPLES, MAGENTA, (unsigned long)g_pc_samples, RESET, sizeof(g_pc_samples), CYAN, RESET);
    printf("%s║%s  │  g_syscall_table[32]      %s0x%014lx%s   %5zu   tabla de syscalls        │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)g_syscall_table, RESET, sizeof(g_syscall_table), CYAN, RESET);
    printf("%s║%s  │  g_heap_log[32]           %s0x%014lx%s   %5zu   registro del heap        │%s║%s\n",
           CYAN, RESET, MAGENTA, (unsigned long)g_heap_log, RESET, sizeof(g_heap_log), CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %sNota:%s En la version secuencial no hay riesgo de data race porque solo hay 1 hilo.%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  Las variables globales son seguras sin necesidad de mutex o atomics.                %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  4. MECANISMO DE TRAPS (Interrupciones Software)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_trap_demo(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *RED    = "\033[31m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sMECANISMO DE TRAPS (Interrupciones Software)%s                              %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ Tipos de Traps ──────────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %s1. Syscall Trap:%s El programa invoca voluntariamente al kernel (read, write,   │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │    mmap, etc.) mediante instruccion SVC (ARM64) o SYSCALL (x86_64).            │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %s2. Exception Trap:%s Error de HW: division por cero, acceso invalido a memoria, │%s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │    page fault. El CPU genera la excepcion automaticamente.                     │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %s3. Signal Trap:%s El kernel entrega una senial al proceso (SIGSEGV, SIGFPE,     │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │    SIGINT). Puede tener handler registrado o usar accion por defecto.          │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
#ifdef __APPLE__
    printf("%s║%s  %s┌─ macOS: Mach Traps ───────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ En macOS/Darwin, ademas de POSIX syscalls, existen %sMach Traps%s: llamadas al    │%s║%s\n", CYAN, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s  │ microkernel Mach. Se usan para: task_info(), mach_task_self(),                 │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ thread_info(), mach_vm_allocate(), etc. Son un mecanismo separado de POSIX.    │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ task_info() → Mach Trap para obtener estadisticas de memoria del proceso.      │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
#endif

    /* Tabla de seniales */
    printf("%s║%s  %s┌─ Seniales registradas ────────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  %-10s %-8s %-12s %-10s %-28s │%s║%s\n",
           CYAN, RESET, "Senial", "Numero", "Handler", "Recibidas", "Descripcion", CYAN, RESET);
    printf("%s║%s  │  ────────── ──────── ──────────── ────────── ──────────────────────────── │%s║%s\n", CYAN, RESET, CYAN, RESET);

    int total_signals = 0;
    for (int i = 0; i < g_num_signals; i++) {
        printf("%s║%s  │  %s%-10s%s %-8d %s%-12s%s %s%-10d%s %-28s │%s║%s\n",
               CYAN, RESET,
               YELLOW, g_signal_table[i].name, RESET,
               g_signal_table[i].signum,
               GREEN, "Registrado", RESET,
               MAGENTA, (int)g_signal_table[i].received_count, RESET,
               g_signal_table[i].description,
               CYAN, RESET);
        total_signals += (int)g_signal_table[i].received_count;
    }
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Diagrama de transicion User <-> Kernel */
    printf("%s║%s  %s┌─ Transicion User Mode <-> Kernel Mode ────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │   %s┌──────────────┐%s         TRAP/SVC          %s┌──────────────┐%s                   │%s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │   │  %sUser Mode%s   │ ═══════════════════> │  %sKernel Mode%s │                   │%s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │   │  (programa)  │ <═══════════════════ │  (SO kernel) │                   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │   %s└──────────────┘%s        IRET/ERET         %s└──────────────┘%s                   │%s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Resumen */
    printf("%s║%s  %sResumen:%s POSIX syscalls: %s%d%s | Mach traps: %s~3%s (task_info, thread_info)            %s║%s\n",
           CYAN, RESET, WHITE, g_num_tracked_syscalls, GREEN, g_num_tracked_syscalls, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s           Seniales recibidas: %s%d%s | Excepciones HW: %s0%s (ejecucion normal)          %s║%s\n",
           CYAN, RESET, YELLOW, total_signals, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  5. INTERRUPCIONES Y CAMBIOS DE CONTEXTO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_interrupts_info(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *RED    = "\033[31m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    /* Obtener deltas de rusage */
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long delta_vcsw  = ru.ru_nvcsw  - g_baseline_vcsw;
    long delta_ivcsw = ru.ru_nivcsw - g_baseline_ivcsw;
    long delta_minflt = ru.ru_minflt - g_baseline_minflt;
    long delta_majflt = ru.ru_majflt - g_baseline_majflt;

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sINTERRUPCIONES Y CAMBIOS DE CONTEXTO%s                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ Tipos de Interrupciones ─────────────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %sHW Interrupts:%s Timer (quantum expiro), I/O (disco/red completo),              │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │   IPI (Inter-Processor Interrupt para sincronizar cores).                      │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %sSW Interrupts:%s Syscalls (programa solicita servicio), Page faults (acceso a   │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │   pagina no mapeada), excepciones aritmeticas (div/0, overflow).               │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla de getrusage deltas */
    printf("%s║%s  %s┌─ Estadisticas del proceso (getrusage delta) ──────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  %-35s %s%10ld%s                              │%s║%s\n",
           CYAN, RESET, "Cambios de contexto voluntarios:", GREEN, delta_vcsw, RESET, CYAN, RESET);
    printf("%s║%s  │    %s-> Causa: bloqueo por I/O (read, write, fopen, etc.)%s                       │%s║%s\n",
           CYAN, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s  │  %-35s %s%10ld%s                              │%s║%s\n",
           CYAN, RESET, "Cambios de contexto involuntarios:", YELLOW, delta_ivcsw, RESET, CYAN, RESET);
    printf("%s║%s  │    %s-> Causa: timer interrupt (quantum expiro, scheduler preemptivo)%s           │%s║%s\n",
           CYAN, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s  │  %-35s %s%10ld%s                              │%s║%s\n",
           CYAN, RESET, "Minor page faults:", GREEN, delta_minflt, RESET, CYAN, RESET);
    printf("%s║%s  │    %s-> Causa: pagina en memoria pero no mapeada (demand paging, COW)%s          │%s║%s\n",
           CYAN, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s  │  %-35s %s%10ld%s                              │%s║%s\n",
           CYAN, RESET, "Major page faults:", RED, delta_majflt, RESET, CYAN, RESET);
    printf("%s║%s  │    %s-> Causa: pagina en disco (swap), requiere I/O de disco%s                    │%s║%s\n",
           CYAN, RESET, MAGENTA, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Explicacion adicional */
    printf("%s║%s  %s┌─ Que significan estos numeros? ───────────────────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %sVoluntarios altos:%s El proceso hizo mucho I/O (archivos, red).                 │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │ %sInvoluntarios altos:%s El scheduler interrumpio el proceso frecuentemente       │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │   (mucha competencia por CPU o quantum pequenio).                              │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %sMinor faults altos:%s El proceso toco muchas paginas nuevas (heap grande,       │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │   primera lectura de datos mapeados). Es normal para compresion de imagenes.   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %sMajor faults > 0:%s Hubo acceso a paginas en swap (presion de memoria).         │%s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  6. MANEJO DE ERRORES (Error Handling)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_error_handling_demo(void) {
    const char *CYAN   = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN  = "\033[32m";
    const char *WHITE  = "\033[1;37m";
    const char *RED    = "\033[31m";
    const char *MAGENTA= "\033[35m";
    const char *RESET  = "\033[0m";

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s           %sMANEJO DE ERRORES (Error Handling)%s                                        %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ 3 Mecanismos de manejo de errores en C/POSIX ─────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │ %s1. errno:%s Variable global thread-local. Despues de una syscall fallida,        │%s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │    errno contiene el codigo de error. strerror(errno) da descripcion legible.  │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %s2. Signal handlers:%s El kernel envia seniales ante eventos excepcionales.       │%s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │    Se registran con sigaction(). Ejemplo: SIGSEGV ante acceso invalido.        │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │ %s3. Return codes:%s Las funciones retornan codigos de error (NULL, -1, etc.).     │%s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │    El programa debe verificar cada retorno y actuar en consecuencia.           │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Demo de errno */
    printf("%s║%s  %s┌─ Demo errno: intentar abrir archivo inexistente ──────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    errno = 0;
    FILE *bad = fopen("/nonexistent_rle_demo", "r");
    int saved_errno = errno;
    if (bad) fclose(bad); /* No deberia pasar */
    printf("%s║%s  │  fopen(\"/nonexistent_rle_demo\", \"r\") → %sNULL%s                                  │%s║%s\n",
           CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  │  errno = %s%d%s (%s%s%s)                                                            │%s║%s\n",
           CYAN, RESET, MAGENTA, saved_errno, RESET, RED, strerror(saved_errno), RESET, CYAN, RESET);
    printf("%s║%s  │  Significado: %sENOENT%s = El archivo o directorio no existe.                     │%s║%s\n",
           CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  │  El kernel retorna este error via la syscall open() al no encontrar el path.   │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Tabla de puntos de manejo de errores en el programa */
    printf("%s║%s  %s┌─ Puntos de manejo de errores en este programa ────────────────────────────────┐%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %-14s %-10s │%s║%s\n",
           CYAN, RESET, "Funcion", "Error posible", "Accion", "Severidad", CYAN, RESET);
    printf("%s║%s  │  ──────────────────── ────────────────────── ────────────── ────────── │%s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "malloc()", "Sin memoria (NULL)", RED, "exit(1)", RESET, RED, "CRITICA", RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "realloc()", "Sin memoria (NULL)", RED, "exit(1)", RESET, RED, "CRITICA", RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "stbi_load()", "Formato invalido", YELLOW, "return -1", RESET, YELLOW, "ALTA", RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "fopen()", "Archivo no existe", YELLOW, "return/skip", RESET, YELLOW, "MEDIA", RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "rle_decompress()", "Datos corruptos", YELLOW, "return NULL", RESET, YELLOW, "ALTA", RESET, CYAN, RESET);
    printf("%s║%s  │  %-20s %-22s %s%-14s%s %s%-10s%s │%s║%s\n",
           CYAN, RESET, "memcmp()", "Datos no coinciden", GREEN, "aviso", RESET, GREEN, "BAJA", RESET, CYAN, RESET);
    printf("%s║%s  %s└──────────────────────────────────────────────────────────────────────────────────┘%s%s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Seniales registradas */
    printf("%s║%s  %sHandlers de seniales registrados:%s %s%d seniales%s con handler personalizado          %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, g_num_signals, RESET, CYAN, RESET);
    printf("%s║%s  (SIGSEGV, SIGFPE, SIGBUS, SIGABRT, SIGINT, SIGTERM)                                %s║%s\n", CYAN, RESET, CYAN, RESET);
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
    int used_stb = 0;            /* Flag para saber si liberar con stbi_image_free */
    char input_path[512] = {0};

    /* Cargar imagen: argumento, menú interactivo, o sintética */
    if (argc >= 2) {
        strncpy(input_path, argv[1], sizeof(input_path) - 1);
        if (load_image(input_path, &img) != 0) {
            return 1;
        }
        used_stb = 1;
    } else {
        /* Escanear carpeta image/ y listar imágenes disponibles */
        const char *img_dir = "image";
        char files[64][256];
        int nfiles = 0;

        DIR *dir = opendir(img_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && nfiles < 64) {
                const char *name = entry->d_name;
                size_t len = strlen(name);
                if (len < 5) continue;
                const char *ext = name + len - 4;
                const char *ext3 = name + len - 3;
                if (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
                    strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".gif") == 0 ||
                    strcasecmp(ext, ".tga") == 0 || strcasecmp(ext, ".psd") == 0 ||
                    strcasecmp(ext, ".hdr") == 0 || strcasecmp(ext3, ".jpeg") == 0) {
                    strncpy(files[nfiles], name, 255);
                    files[nfiles][255] = '\0';
                    nfiles++;
                }
            }
            closedir(dir);
        }

        printf("\n\033[1;36m╔══════════════════════════════════════════════════════════════╗\033[0m\n");
        printf("\033[1;36m║\033[0m  \033[1;33mCOMPRESIÓN RLE - MODO SECUENCIAL\033[0m                            \033[1;36m║\033[0m\n");
        printf("\033[1;36m╠══════════════════════════════════════════════════════════════╣\033[0m\n");
        printf("\033[1;36m║\033[0m                                                              \033[1;36m║\033[0m\n");
        printf("\033[1;36m║\033[0m  \033[1;37mImágenes disponibles en /%s/:\033[0m                              \033[1;36m║\033[0m\n", img_dir);
        printf("\033[1;36m║\033[0m                                                              \033[1;36m║\033[0m\n");

        if (nfiles == 0) {
            printf("\033[1;36m║\033[0m    \033[31m(No se encontraron imágenes en /%s/)\033[0m                  \033[1;36m║\033[0m\n", img_dir);
        } else {
            for (int i = 0; i < nfiles; i++) {
                printf("\033[1;36m║\033[0m    \033[1;32m[%d]\033[0m %-50s  \033[1;36m║\033[0m\n", i + 1, files[i]);
            }
        }

        printf("\033[1;36m║\033[0m                                                              \033[1;36m║\033[0m\n");
        printf("\033[1;36m║\033[0m    \033[1;33m[0]\033[0m Usar imagen sintética (4096x4096)                    \033[1;36m║\033[0m\n");
        printf("\033[1;36m║\033[0m    \033[1;35m[B]\033[0m Buscar archivo con explorador de archivos...         \033[1;36m║\033[0m\n");
        printf("\033[1;36m║\033[0m                                                              \033[1;36m║\033[0m\n");
        printf("\033[1;36m╚══════════════════════════════════════════════════════════════╝\033[0m\n");
        printf("\n  Seleccione imagen (número o 'b'): ");
        fflush(stdout);

        char choice[16];
        int sel = 0;
        int use_browser = 0;
        if (fgets(choice, sizeof(choice), stdin)) {
            if (choice[0] == 'b' || choice[0] == 'B') {
                use_browser = 1;
            } else {
                sel = atoi(choice);
            }
        }

        if (use_browser) {
            /* Abrir diálogo nativo de macOS para seleccionar imagen */
            FILE *fp = popen(
                "osascript -e 'set theFile to choose file with prompt "
                "\"Seleccione una imagen para comprimir\" of type "
                "{\"public.png\", \"public.jpeg\", \"public.bmp\", \"public.gif\", "
                "\"public.targa-image\", \"com.adobe.photoshop-image\", "
                "\"public.radiance\"}' "
                "-e 'POSIX path of theFile'", "r");
            if (fp) {
                char browser_path[512] = {0};
                if (fgets(browser_path, sizeof(browser_path), fp)) {
                    /* Quitar salto de línea */
                    size_t blen = strlen(browser_path);
                    if (blen > 0 && browser_path[blen - 1] == '\n')
                        browser_path[blen - 1] = '\0';
                    strncpy(input_path, browser_path, sizeof(input_path) - 1);
                }
                int ret = pclose(fp);
                if (ret != 0 || input_path[0] == '\0') {
                    printf("  \033[33mNo se seleccionó archivo. Usando imagen sintética...\033[0m\n");
                    generate_synthetic(&img, 4096, 4096);
                } else if (load_image(input_path, &img) != 0) {
                    printf("  \033[33mUsando imagen sintética como respaldo...\033[0m\n");
                    generate_synthetic(&img, 4096, 4096);
                } else {
                    used_stb = 1;
                }
            } else {
                printf("  \033[33mNo se pudo abrir el explorador. Usando imagen sintética...\033[0m\n");
                generate_synthetic(&img, 4096, 4096);
            }
        } else if (sel >= 1 && sel <= nfiles) {
            snprintf(input_path, sizeof(input_path), "%s/%s", img_dir, files[sel - 1]);
            if (load_image(input_path, &img) != 0) {
                printf("  \033[33mUsando imagen sintética como respaldo...\033[0m\n");
                generate_synthetic(&img, 4096, 4096);
            } else {
                used_stb = 1;
            }
        } else {
            printf("  \033[33mGenerando imagen sintética 4096x4096...\033[0m\n");
            generate_synthetic(&img, 4096, 4096);
        }
    }

    size_t total_pixels = (size_t)img.width * img.height;
    size_t raw_size = total_pixels * 3;

    /* Guardar archivo RAW (RGB) */
    char rawpath[512];
    if (input_path[0])
        snprintf(rawpath, sizeof(rawpath), "%s.raw", input_path);
    else
        snprintf(rawpath, sizeof(rawpath), "output.raw");

    FILE *fraw = fopen(rawpath, "wb");
    if (fraw) {
        fwrite(img.data, 1, raw_size, fraw);
        fclose(fraw);
        printf("  \033[32mArchivo RAW guardado:\033[0m %s (%.2f KB)\n", rawpath, raw_size / 1024.0);
    }

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
    g_t0_compress = t_start;  /* Tiempo base para muestreo del PC */
    g_num_pc_samples = 0;

    /* COMPRESIÓN */
    rle_compress(img.data, raw_size, &compressed, &prog);

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
    if (input_path[0])
        snprintf(outpath, sizeof(outpath), "%s_secuencial.rle", input_path);
    else
        snprintf(outpath, sizeof(outpath), "output_secuencial.rle");

    FILE *fout = fopen(outpath, "wb");
    if (fout) {
        uint32_t header[2] = { img.width, img.height };
        fwrite(header, sizeof(uint32_t), 2, fout);
        fwrite(compressed.data, 1, compressed.size, fout);
        fclose(fout);
        printf("\n  \033[32mArchivo comprimido guardado:\033[0m %s\n", outpath);
    }

    /* ═══════════════════════════════════════════════════════════════════
     *  DESCOMPRESIÓN Y GENERACIÓN DE IMAGEN BMP
     * ═══════════════════════════════════════════════════════════════════ */
    printf("\n\033[33m  Descomprimiendo datos RLE...\033[0m\n");

    struct timespec td_start, td_end;
    clock_gettime(CLOCK_MONOTONIC, &td_start);

    uint8_t *decoded = rle_decompress(compressed.data, compressed.size, raw_size);

    clock_gettime(CLOCK_MONOTONIC, &td_end);
    double decomp_time = (td_end.tv_sec - td_start.tv_sec) +
                         (td_end.tv_nsec - td_start.tv_nsec) / 1e9;

    if (decoded) {
        /* Verificar integridad: comparar con imagen original */
        int match = (memcmp(decoded, img.data, raw_size) == 0);

        /* Guardar como BMP */
        char bmppath[512];
        if (input_path[0])
            snprintf(bmppath, sizeof(bmppath), "%s_secuencial_descomprimida.bmp", input_path);
        else
            snprintf(bmppath, sizeof(bmppath), "output_secuencial_descomprimida.bmp");

        save_bmp(bmppath, decoded, img.width, img.height);

        const char *CYAN = "\033[36m";
        const char *GREEN = "\033[32m";
        const char *YELLOW = "\033[33m";
        const char *WHITE = "\033[1;37m";
        const char *RED = "\033[31m";
        const char *RESET = "\033[0m";

        printf("\n");
        printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
        printf("%s║%s                    %s*** DESCOMPRESIÓN Y VERIFICACIÓN ***%s                          %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
        printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
        printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
        printf("%s║%s  %sTiempo de descompresión:%s  %s%12.6f%s segundos                                     %s║%s\n",
               CYAN, RESET, WHITE, RESET, GREEN, decomp_time, RESET, CYAN, RESET);
        printf("%s║%s  %sPíxeles decodificados:%s    %s%12zu%s                                              %s║%s\n",
               CYAN, RESET, WHITE, RESET, GREEN, total_pixels, RESET, CYAN, RESET);
        printf("%s║%s  %sBytes decodificados:%s      %s%12zu%s (%.2f MB)                                    %s║%s\n",
               CYAN, RESET, WHITE, RESET, GREEN, raw_size, RESET, raw_size / (1024.0 * 1024.0), CYAN, RESET);
        printf("%s║%s  %sIntegridad:%s               %s%s%s                                                    %s║%s\n",
               CYAN, RESET, WHITE, RESET,
               match ? GREEN : RED,
               match ? "CORRECTA - Imagen idéntica al original" : "ERROR - Diferencias detectadas",
               RESET, CYAN, RESET);
        printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
        printf("%s║%s  %sImagen descomprimida:%s     %s%-50s%s     %s║%s\n",
               CYAN, RESET, WHITE, RESET, GREEN, bmppath, RESET, CYAN, RESET);
        printf("%s║%s  %sFormato:%s                  BMP (24-bit RGB, sin compresión)                       %s║%s\n",
               CYAN, RESET, WHITE, RESET, CYAN, RESET);
        printf("%s║%s  %sDimensiones:%s              %u x %u px                                             %s║%s\n",
               CYAN, RESET, WHITE, RESET, img.width, img.height, CYAN, RESET);
        printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
        printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n\n", CYAN, RESET);

        free(decoded);
    } else {
        printf("  \033[31mError: No se pudo descomprimir los datos RLE.\033[0m\n\n");
    }

    /* ═══════════════════════════════════════════════════════════════════
     *  EXPORTAR CSV CON DATOS DE SCHEDULING PARA DIAGRAMA DE GANTT
     * ═══════════════════════════════════════════════════════════════════ */
    {
        char csvpath[512];
        if (input_path[0])
            snprintf(csvpath, sizeof(csvpath), "%s_secuencial_gantt.csv", input_path);
        else
            snprintf(csvpath, sizeof(csvpath), "output_secuencial_gantt.csv");

        uint64_t main_tid = 0;
#ifdef __APPLE__
        pthread_threadid_np(NULL, &main_tid);
#endif

        FILE *csv = fopen(csvpath, "w");
        if (csv) {
            fprintf(csv, "algorithm,pid,num_threads,wall_ms,total_cpu_ms\n");
            fprintf(csv, "secuencial,%d,1,%.4f,%.4f\n",
                    getpid(), elapsed * 1000, (user_t + sys_t) * 1000);
            fprintf(csv, "\n");

            fprintf(csv, "thread_id,tid,core,start_ms,end_ms,cpu_user_ms,cpu_sys_ms,pixels,compressed_bytes,stack_addr\n");
            fprintf(csv, "0,0x%lx,0,0.0000,%.4f,%.4f,%.4f,%zu,%zu,0x%lx\n",
                    (unsigned long)main_tid,
                    elapsed * 1000,
                    user_t * 1000, sys_t * 1000,
                    total_pixels, compressed.size,
                    (unsigned long)&stack_marker_top);
            fprintf(csv, "\n");

            /* Muestras reales del PC durante la compresión */
            fprintf(csv, "thread_id,sample_idx,timestamp_ms,pc_addr,core_id,pixels_at\n");
            for (int s = 0; s < g_num_pc_samples; s++) {
                fprintf(csv, "0,%d,%.4f,0x%lx,0,%zu\n",
                        s, g_pc_samples[s].timestamp_ms,
                        (unsigned long)g_pc_samples[s].pc_addr,
                        g_pc_samples[s].pixels_at);
            }

            fclose(csv);
            printf("  \033[32mDatos de scheduling exportados:\033[0m %s\n\n", csvpath);
        }
    }

    /* Liberar memoria del HEAP */
    if (used_stb)
        stbi_image_free(img.data);  /* stb_image usa su propio allocator */
    else
        free(img.data);
    free(compressed.data);
    return 0;
}
