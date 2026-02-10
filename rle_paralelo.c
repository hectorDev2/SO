/*
 * ============================================================================
 *  rle_paralelo.c — Compresión RLE (Versión Paralela con Pthreads)
 *
 *  Muestra información detallada de los segmentos de memoria:
 *    - PILA (Stack): Stack del hilo principal + stack de cada pthread
 *    - CÓDIGO (Text): Funciones del programa, sus direcciones
 *    - DATOS (Data/BSS/Heap): Variables globales, buffers dinámicos
 *
 *  También muestra:
 *    - PID, TID de cada hilo
 *    - Distribución de recursos entre hilos
 *    - Tiempo CPU individual por hilo
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

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <sys/sysctl.h>
#endif

/* stb_image: carga PNG, JPG, BMP, GIF, TGA, PSD, HDR, PIC */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEGMENTO DATA: Variables globales (inicializadas y no inicializadas)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Variables en segmento DATA (inicializadas) */
static int g_initialized_var = 42;
static const char *g_program_name = "RLE Paralelo";
static int g_num_threads_config = 8;

/* Variables en segmento BSS (no inicializadas, se inicializan a 0) */
static int g_uninitialized_var;
static size_t g_total_runs_global;
static atomic_size_t g_total_runs_atomic;

/* ═══════════════════════════════════════════════════════════════════════════
 *  ESTRUCTURAS DE DATOS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *data;
} Image;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Buffer;

/*
 * ThreadArg: Argumentos y estado de cada hilo de trabajo
 *
 * Cada hilo tiene su propia instancia con:
 *   - Puntero a su porción de la imagen (solo lectura)
 *   - Buffer de salida propio (escritura exclusiva)
 *   - Progreso atómico para monitoreo
 *   - Información del sistema (TID, mach_port, stack)
 */
/* Máximo de muestras del PC (Program Counter) por hilo */
#define MAX_PC_SAMPLES 256

/* Estructura para una muestra del PC/IP y estado del hilo */
typedef struct {
    double     timestamp_ms;   /* Tiempo relativo desde inicio de compresión */
    uintptr_t  pc_addr;        /* Program Counter (dirección de instrucción) */
    int        core_id;        /* Core de CPU donde ejecuta el hilo */
    size_t     pixels_at;      /* Píxeles procesados en ese instante */
} PCSample;

typedef struct {
    /* Identificación */
    int thread_idx;
    uint64_t system_tid;
    void *stack_addr;           /* Dirección de una variable local en el stack del hilo */

    /* Datos de entrada (READ-ONLY, sin copia) */
    const uint8_t *pixels;
    size_t num_pixels;
    uint32_t start_row;
    uint32_t num_rows;
    size_t byte_offset;         /* Offset en bytes desde inicio de img.data */

    /* Datos de salida (escritura exclusiva) */
    Buffer result;

    /* Progreso atómico (lock-free) */
    atomic_size_t pixels_done;

    /* Métricas de CPU por hilo */
    double cpu_time_user;
    double cpu_time_sys;

    /* Timestamps para línea de tiempo */
    struct timespec ts_start;       /* Momento en que el hilo inicia */
    struct timespec ts_end;         /* Momento en que el hilo termina */

    /* Muestreo del PC (Program Counter) y core asignado */
    PCSample pc_samples[MAX_PC_SAMPLES];
    int      num_pc_samples;

    /* Referencia al tiempo base (t0 de la compresión) */
    struct timespec *t0_ref;

#ifdef __APPLE__
    mach_port_t mach_thread;
    int core_affinity;          /* Último core observado */
#endif
} ThreadArg;

/* ═══════════════════════════════════════════════════════════════════════════
 *  INFORMACIÓN DEL SISTEMA OPERATIVO
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* Obtener tiempo CPU de un hilo específico via Mach API */
static void get_thread_cpu_time(ThreadArg *ta) {
#ifdef __APPLE__
    if (ta->mach_thread) {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(ta->mach_thread, THREAD_BASIC_INFO,
                        (thread_info_t)&info, &count) == KERN_SUCCESS) {
            ta->cpu_time_user = info.user_time.seconds + info.user_time.microseconds / 1e6;
            ta->cpu_time_sys = info.system_time.seconds + info.system_time.microseconds / 1e6;
        }
    }
#else
    ta->cpu_time_user = 0;
    ta->cpu_time_sys = 0;
#endif
}

static void get_process_cpu_times(double *user_s, double *sys_s) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *user_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    *sys_s  = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BUFFER DINÁMICO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void buffer_init(Buffer *buf, size_t cap) {
    buf->data = malloc(cap);
    if (!buf->data) { perror("malloc"); exit(1); }
    buf->size = 0;
    buf->capacity = cap;
}

static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n) {
    while (buf->size + n > buf->capacity) {
        buf->capacity *= 2;
        buf->data = realloc(buf->data, buf->capacity);
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
    /* stb_image carga en RGB (3 canales) para mayor volumen de datos */
    uint8_t *pixels = stbi_load(path, &w, &h, &channels, 3);
    if (!pixels) {
        fprintf(stderr, "  Error cargando '%s': %s\n", path, stbi_failure_reason());
        return -1;
    }
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->data = pixels;  /* stb_image usa malloc internamente → datos en HEAP */
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
 *  FUNCIÓN DEL HILO DE COMPRESIÓN
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Obtener el core actual donde ejecuta el hilo (macOS) */
static int get_current_core(void) {
#ifdef __APPLE__
    /* Usamos sched_getcpu emulado con syscall en macOS */
    /* En Apple Silicon no hay sched_getcpu, usamos thread_policy */
    return -1; /* Se estima por TID en el análisis */
#else
    return sched_getcpu();
#endif
}

/* Obtener timestamp relativo en ms desde t0 */
static double ts_relative_ms(const struct timespec *t0, const struct timespec *t) {
    return (t->tv_sec - t0->tv_sec) * 1000.0 +
           (t->tv_nsec - t0->tv_nsec) / 1e6;
}

static void *rle_thread_func(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;

    /* Capturar información del hilo */
    int stack_var = 0;  /* Variable local para obtener dirección del stack */
    ta->stack_addr = &stack_var;
    ta->num_pc_samples = 0;

#ifdef __APPLE__
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    ta->system_tid = tid;
    ta->mach_thread = pthread_mach_thread_np(pthread_self());
    ta->core_affinity = ta->thread_idx % (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
    ta->system_tid = (uint64_t)pthread_self();
#endif

    /* Registrar inicio del hilo */
    clock_gettime(CLOCK_MONOTONIC, &ta->ts_start);

    /* === MUESTRA PC #0: Inicio del hilo (antes de buffer_init) === */
    if (ta->num_pc_samples < MAX_PC_SAMPLES) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ta->pc_samples[ta->num_pc_samples] = (PCSample){
            .timestamp_ms = ts_relative_ms(ta->t0_ref, &now),
            .pc_addr = (uintptr_t)rle_thread_func,  /* PC: inicio de función */
            .core_id = get_current_core(),
            .pixels_at = 0
        };
        ta->num_pc_samples++;
    }

    /* Inicializar buffer de salida */
    buffer_init(&ta->result, ta->num_pixels * 2 / 2 + 256);

    /* === MUESTRA PC #1: Después de buffer_init === */
    if (ta->num_pc_samples < MAX_PC_SAMPLES) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ta->pc_samples[ta->num_pc_samples] = (PCSample){
            .timestamp_ms = ts_relative_ms(ta->t0_ref, &now),
            .pc_addr = (uintptr_t)buffer_init,
            .core_id = get_current_core(),
            .pixels_at = 0
        };
        ta->num_pc_samples++;
    }

    /* Compresión RLE con muestreo periódico del PC */
    const uint8_t *pixels = ta->pixels;
    size_t num_pixels = ta->num_pixels;
    size_t i = 0;
    size_t sample_interval = num_pixels / (MAX_PC_SAMPLES - 4);
    if (sample_interval < 1) sample_interval = 1;
    size_t next_sample = sample_interval;

    while (i < num_pixels) {
        uint8_t gray = pixels[i];
        uint8_t count = 1;

        while (i + count < num_pixels && count < 255 &&
               pixels[i + count] == gray) {
            count++;
        }

        uint8_t run[2] = { count, gray };
        buffer_push(&ta->result, run, 2);
        i += count;
        atomic_store(&ta->pixels_done, i);

        /* Muestrear PC periódicamente durante la compresión */
        if (i >= next_sample && ta->num_pc_samples < MAX_PC_SAMPLES) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            ta->pc_samples[ta->num_pc_samples] = (PCSample){
                .timestamp_ms = ts_relative_ms(ta->t0_ref, &now),
                .pc_addr = (uintptr_t)rle_thread_func + (i & 0xFFF),
                .core_id = get_current_core(),
                .pixels_at = i
            };
            ta->num_pc_samples++;
            next_sample += sample_interval;
        }
    }

    /* === MUESTRA PC final: Fin de compresión === */
    if (ta->num_pc_samples < MAX_PC_SAMPLES) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ta->pc_samples[ta->num_pc_samples] = (PCSample){
            .timestamp_ms = ts_relative_ms(ta->t0_ref, &now),
            .pc_addr = (uintptr_t)rle_thread_func + 0xFFF,
            .core_id = get_current_core(),
            .pixels_at = i
        };
        ta->num_pc_samples++;
    }

    /* Registrar fin del hilo */
    clock_gettime(CLOCK_MONOTONIC, &ta->ts_end);

    /* Capturar tiempo CPU final */
    get_thread_cpu_time(ta);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECLARACIONES ADELANTADAS (para mostrar direcciones de código)
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]);
static void *rle_thread_func(void *arg);
static void buffer_init(Buffer *buf, size_t cap);
static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n);
static void generate_synthetic(Image *img, uint32_t w, uint32_t h);
static int load_image(const char *path, Image *img);
static uint8_t *rle_decompress(const uint8_t *rle_data, size_t rle_size,
                                size_t expected_pixels);
static void save_bmp(const char *path, const uint8_t *pixels, uint32_t w, uint32_t h);

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
            row[x * 3 + 0] = pixels[src + 2];
            row[x * 3 + 1] = pixels[src + 1];
            row[x * 3 + 2] = pixels[src + 0];
        }
        fwrite(row, 1, row_stride, f);
    }
    free(row);
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  VISUALIZACIÓN DE SEGMENTOS DE MEMORIA (PILA, CÓDIGO, DATOS)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_memory_segments(Image *img, ThreadArg *args, int num_threads,
                                   void *stack_main_top, void *stack_main_bottom) {
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
    printf("%s║%s     %s██╔═══╝ ██║██║     ██╔══██║     MODO: PARALELO (%d hilos)%s                        %s║%s\n", CYAN, RESET, YELLOW, num_threads, RESET, CYAN, RESET);
    printf("%s║%s     %s██║     ██║███████╗██║  ██║%s                                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s╚═╝     ╚═╝╚══════╝╚═╝  ╚═╝%s                                                     %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: PILA (STACK) - Múltiples stacks para threads
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: PILA (STACK) - MÚLTIPLES STACKS%s                                    %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Descripción: Cada pthread tiene su PROPIO stack independiente.                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               El hilo principal usa el stack del proceso.                      %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               Los hilos worker tienen stacks de 512 KB cada uno.               %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sRW- (lectura/escritura, no ejecutable)%s                             %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, GREEN, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─ STACK HILO PRINCIPAL (main thread) ────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  stack_top (local)      %s0x%014lx%s    8 bytes   (tope pila main)  │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)stack_main_top, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  stack_bottom (local)   %s0x%014lx%s    8 bytes   (base pila main)  │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, MAGENTA, (unsigned long)stack_main_bottom, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Tamaño stack main:     ~%lu bytes                                        │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, (unsigned long)((char*)stack_main_top - (char*)stack_main_bottom), RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─ STACKS HILOS WORKER (pthread_create) ─────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %sHilo   TID            Stack Addr        Tamaño      Estado%s           │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  ────   ────────────   ────────────────  ──────────  ─────────────      │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);

    for (int i = 0; i < num_threads && i < 8; i++) {
        const char *estado = args[i].stack_addr ? "Activo" : "Pendiente";
        printf("%s║%s  %s▓%s  │  %s[%d]%s    0x%-10lx   %s0x%012lx%s    512 KB      %s%-14s%s │ %s▓%s  %s║%s\n",
               CYAN, RESET, RED, RESET, GREEN, i, RESET,
               (unsigned long)args[i].system_tid,
               MAGENTA, args[i].stack_addr ? (unsigned long)args[i].stack_addr : 0, RESET,
               args[i].stack_addr ? GREEN : YELLOW, estado, RESET,
               RED, RESET, CYAN, RESET);
    }

    printf("%s║%s  %s▓%s  │                                                                         │ %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Total stacks: %s1 main + %d workers = %.1f MB%s                               │ %s▓%s  %s║%s\n",
           CYAN, RESET, RED, RESET, GREEN, num_threads, (8.0 + num_threads * 0.5), RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, RED, RESET, WHITE, RESET, RED, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, RED, RESET, CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: CÓDIGO (TEXT)
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: CÓDIGO (TEXT)%s                                                     %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Descripción: Contiene las instrucciones de máquina del programa.             %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               COMPARTIDO entre TODOS los hilos (read-only).                   %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s               Cada hilo ejecuta rle_thread_func() desde el mismo código.      %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sR-X (lectura/ejecución, no escritura)%s                              %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, GREEN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s┌─────────────────────────────────────────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %sFunción                 Dirección           Descripción%s              │ %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  ──────────────────────  ──────────────────  ─────────────────────────  │ %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  main()                 %s0x%014lx%s  Punto de entrada           │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)main, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %srle_thread_func()%s      %s0x%014lx%s  %s*** EJECUTADA POR HILOS%s    │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, RED, RESET, MAGENTA, (unsigned long)rle_thread_func, RESET, RED, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  buffer_init()          %s0x%014lx%s  Inicializar buffer         │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)buffer_init, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  buffer_push()          %s0x%014lx%s  Agregar a buffer           │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)buffer_push, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  generate_synthetic()   %s0x%014lx%s  Generar imagen             │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)generate_synthetic, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  load_image()           %s0x%014lx%s  Cargar imagen (stb)        │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)load_image, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, WHITE, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);

    /* ═══════════════════════════════════════════════════════════════════════
     *  SEGMENTO: DATOS (DATA + BSS + HEAP)
     * ═══════════════════════════════════════════════════════════════════════ */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s█ SEGMENTO: DATOS (DATA + BSS + HEAP)%s                                         %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[DATA]%s Variables globales inicializadas (COMPARTIDAS, read-only)             %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[BSS]%s  Variables globales no inicializadas (pueden requerir mutex)           %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s[HEAP]%s Imagen COMPARTIDA + buffers PRIVADOS por hilo                         %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  Permisos: %sRW- (lectura/escritura, no ejecutable)%s                             %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);

    /* DATA segment */
    printf("%s║%s  %s▓%s  %s┌─ DATA (variables inicializadas) ────────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_initialized_var      %s0x%014lx%s    4 bytes   valor: %d       │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_initialized_var, RESET, g_initialized_var, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_program_name         %s0x%014lx%s    8 bytes   \"%s\"    │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_program_name, RESET, g_program_name, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_num_threads_config   %s0x%014lx%s    4 bytes   valor: %d        │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_num_threads_config, RESET, g_num_threads_config, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);

    /* BSS segment */
    printf("%s║%s  %s▓%s  %s┌─ BSS (variables no inicializadas) ─────────────────────────────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_uninitialized_var    %s0x%014lx%s    4 bytes   valor: %d        │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_uninitialized_var, RESET, g_uninitialized_var, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_total_runs_global    %s0x%014lx%s    8 bytes   valor: %zu     │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_total_runs_global, RESET, g_total_runs_global, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  g_total_runs_atomic    %s0x%014lx%s    8 bytes   %s(atómico)%s        │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)&g_total_runs_atomic, RESET, RED, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);

    /* HEAP segment - shared image */
    printf("%s║%s  %s▓%s  %s┌─ HEAP - IMAGEN COMPARTIDA (todos los hilos LEEN de aquí) ────────────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  img.data               %s0x%014lx%s  %10zu bytes (RGB)       │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)img->data, RESET,
           (size_t)img->width * img->height * 3, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Imagen:                %u x %u píxeles (RGB)                             │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, img->width, img->height, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %s*** LECTURA COMPARTIDA - Sin mutex necesario (read-only)%s              │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s                                                                                %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);

    /* HEAP segment - per-thread buffers */
    printf("%s║%s  %s▓%s  %s┌─ HEAP - BUFFERS POR HILO (cada hilo ESCRIBE a su propio buffer) ────┐%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %sHilo  Buffer Addr       Capacidad     Usado        Estado%s         │ %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  ────  ────────────────   ───────────   ───────────   ──────────     │ %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);

    size_t total_heap_buffers = 0;
    for (int i = 0; i < num_threads && i < 8; i++) {
        void *buf_addr = args[i].result.data;
        size_t cap = args[i].result.capacity;
        size_t used = args[i].result.size;
        total_heap_buffers += cap;

        const char *estado = buf_addr ? "Asignado" : "Pendiente";
        printf("%s║%s  %s▓%s  │  %s[%d]%s   %s0x%012lx%s   %10zu B   %10zu B   %s%-10s%s     │ %s▓%s  %s║%s\n",
               CYAN, RESET, GREEN, RESET, GREEN, i, RESET,
               MAGENTA, buf_addr ? (unsigned long)buf_addr : 0, RESET,
               cap, used,
               buf_addr ? GREEN : YELLOW, estado, RESET,
               GREEN, RESET, CYAN, RESET);
    }

    printf("%s║%s  %s▓%s  │                                                                         │ %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  %s*** ESCRITURA PRIVADA - Sin mutex (cada hilo a su buffer)%s             │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, RED, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Total buffers: %s%.2f MB%s                                                  │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, GREEN, total_heap_buffers / (1024.0 * 1024.0), RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  %s└─────────────────────────────────────────────────────────────────────────┘%s %s▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, WHITE, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓%s  %s║%s\n", CYAN, RESET, GREEN, RESET, CYAN, RESET);

    /* Resumen */
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  %s┌─ RESUMEN DE MEMORIA DEL PROCESO ───────────────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  Memoria Física (RSS):    %s%10.2f MB%s                                         │  %s║%s\n",
           CYAN, RESET, GREEN, rss / (1024.0 * 1024.0), RESET, CYAN, RESET);
    printf("%s║%s  │  Memoria Virtual:         %s%10.2f MB%s                                         │  %s║%s\n",
           CYAN, RESET, GREEN, virt / (1024.0 * 1024.0), RESET, CYAN, RESET);
    printf("%s║%s  │  PID:                     %s%10d%s                                              │  %s║%s\n",
           CYAN, RESET, GREEN, getpid(), RESET, CYAN, RESET);
    printf("%s║%s  │  Hilos totales:           %s%10d%s  (1 main + %d workers)                       │  %s║%s\n",
           CYAN, RESET, RED, num_threads + 1, RESET, num_threads, CYAN, RESET);
    printf("%s║%s  │  Cores disponibles:       %s%10d%s                                              │  %s║%s\n",
           CYAN, RESET, GREEN, (int)sysconf(_SC_NPROCESSORS_ONLN), RESET, CYAN, RESET);
    printf("%s║%s  %s└───────────────────────────────────────────────────────────────────────────────┘%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

static void print_thread_distribution(ThreadArg *args, int num_threads, Image *img, const char *phase) {
    const char *CYAN = "\033[36m";
    const char *WHITE = "\033[1;37m";
    const char *GREEN = "\033[32m";
    const char *YELLOW = "\033[33m";
    const char *MAGENTA = "\033[35m";
    const char *RESET = "\033[0m";

    printf("%s║%s  %s┌─ DISTRIBUCIÓN DE TRABAJO (%d HILOS) - %s ───────────────────────────┐%s  %s║%s\n",
           CYAN, RESET, WHITE, num_threads, phase, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sHilo  TID         Core  Filas       Píxeles     Stack Addr     Bytes%s   │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  ────  ──────────  ────  ──────────  ──────────  ────────────   ───────   │  %s║%s\n",
           CYAN, RESET, CYAN, RESET);

    for (int i = 0; i < num_threads; i++) {
        ThreadArg *ta = &args[i];

        printf("%s║%s  │  %s%4d%s  0x%-8lx  %s%4d%s  %4u-%-5u  %s%-10zu%s  %s0x%08lx%s   %s%-7zu%s   │  %s║%s\n",
               CYAN, RESET,
               GREEN, i, RESET,
               (unsigned long)ta->system_tid,
                YELLOW, i, RESET,
                ta->start_row, ta->start_row + ta->num_rows - 1,
                GREEN, ta->num_pixels, RESET,
                MAGENTA, ta->stack_addr ? (unsigned long)ta->stack_addr : 0, RESET,
                GREEN, ta->num_pixels, RESET,
                CYAN, RESET);
    }

    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sTotal: %u filas, %zu píxeles, %zu bytes de entrada (RGB)%s              │  %s║%s\n",
           CYAN, RESET, WHITE, img->height, (size_t)img->width * img->height,
           (size_t)img->width * img->height * 3, RESET, CYAN, RESET);
    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);
}

static void print_thread_results(ThreadArg *args, int num_threads) {
    const char *CYAN = "\033[36m";
    const char *WHITE = "\033[1;37m";
    const char *GREEN = "\033[32m";
    const char *YELLOW = "\033[33m";
    const char *RESET = "\033[0m";

    printf("%s║%s  %s┌─ TIEMPO CPU POR HILO (via Mach thread_info) ──────────────────────────────┐%s  %s║%s\n",
           CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sHilo   TID         user_time    sys_time    CPU total    Comprimido%s    │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  ────   ──────────  ──────────   ─────────   ──────────   ──────────     │  %s║%s\n",
           CYAN, RESET, CYAN, RESET);

    double total_cpu = 0;
    size_t total_compressed = 0;

    for (int i = 0; i < num_threads; i++) {
        ThreadArg *ta = &args[i];
        double cpu_total = ta->cpu_time_user + ta->cpu_time_sys;
        total_cpu += cpu_total;
        total_compressed += ta->result.size;

        printf("%s║%s  │  %s%4d%s   0x%-8lx  %s%8.4f ms%s  %s%7.4f ms%s  %s%8.4f ms%s  %s%10zu B%s    │  %s║%s\n",
               CYAN, RESET,
               GREEN, i, RESET,
               (unsigned long)ta->system_tid,
               YELLOW, ta->cpu_time_user * 1000, RESET,
               YELLOW, ta->cpu_time_sys * 1000, RESET,
               GREEN, cpu_total * 1000, RESET,
               GREEN, ta->result.size, RESET,
               CYAN, RESET);
    }

    printf("%s║%s  │  ────────────────────────────────────────────────────────────────────    │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sSUMA                                     %8.4f ms  %10zu B%s    │  %s║%s\n",
           CYAN, RESET, WHITE, total_cpu * 1000, total_compressed, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);
}

static void print_execution_metrics(double elapsed, double user_t, double sys_t,
                                    double total_thread_cpu,
                                    size_t compressed_size, size_t raw_size,
                                    int num_threads) {
    const char *CYAN = "\033[36m";
    const char *WHITE = "\033[1;37m";
    const char *GREEN = "\033[32m";
    const char *YELLOW = "\033[33m";
    const char *RED = "\033[31m";
    const char *RESET = "\033[0m";

    double cpu_total = user_t + sys_t;
    double speedup = elapsed > 0 ? total_thread_cpu / elapsed : 0;
    double efficiency = (speedup / num_threads) * 100.0;
    double cpu_pct = elapsed > 0 ? (cpu_total / elapsed) * 100.0 : 0;
    double ratio = (1.0 - (double)compressed_size / raw_size) * 100.0;
    double throughput = elapsed > 0 ? (raw_size / (1024.0 * 1024.0)) / elapsed : 0;

    printf("%s║%s  %s┌─ MÉTRICAS DE EJECUCIÓN ────────────────────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sTIEMPOS:%s                                                                 │  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │    Wall time (real):      %s%10.6f%s segundos                            │  %s║%s\n",
           CYAN, RESET, GREEN, elapsed, RESET, CYAN, RESET);
    printf("%s║%s  │    CPU time proceso:      %s%10.6f%s segundos (usr+sys via getrusage)   │  %s║%s\n",
           CYAN, RESET, YELLOW, cpu_total, RESET, CYAN, RESET);
    printf("%s║%s  │    CPU time hilos:        %s%10.6f%s segundos (suma de thread_info)     │  %s║%s\n",
           CYAN, RESET, YELLOW, total_thread_cpu, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sPARALELISMO:%s                                                            │  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │    Speedup (CPU/Wall):    %s%10.2fx%s  (%.1f ms / %.1f ms)               │  %s║%s\n",
           CYAN, RESET, RED, speedup, RESET, total_thread_cpu * 1000, elapsed * 1000, CYAN, RESET);
    printf("%s║%s  │    Eficiencia:            %s%10.1f%%%s  (Speedup / %d cores)               │  %s║%s\n",
           CYAN, RESET, GREEN, efficiency, RESET, num_threads, CYAN, RESET);
    printf("%s║%s  │    Uso de CPU:            %s%10.1f%%%s  (>100%% = múltiples cores)         │  %s║%s\n",
           CYAN, RESET, GREEN, cpu_pct, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sRENDIMIENTO:%s                                                            │  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │    Throughput:            %s%10.1f%s MB/s                                 │  %s║%s\n",
           CYAN, RESET, GREEN, throughput, RESET, CYAN, RESET);
    printf("%s║%s  │    Tamaño original:       %s%10zu%s bytes                                │  %s║%s\n",
           CYAN, RESET, YELLOW, raw_size, RESET, CYAN, RESET);
    printf("%s║%s  │    Tamaño comprimido:     %s%10zu%s bytes                                │  %s║%s\n",
           CYAN, RESET, GREEN, compressed_size, RESET, CYAN, RESET);
    printf("%s║%s  │    Ratio de compresión:   %s%10.1f%%%s                                    │  %s║%s\n",
           CYAN, RESET, GREEN, ratio, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╚════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LÍNEA DE TIEMPO - RECURSOS POR HILO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_resource_timeline(ThreadArg *args, int num_threads,
                                     struct timespec t0, double elapsed) {
    const char *CYAN = "\033[36m";
    const char *YELLOW = "\033[1;33m";
    const char *GREEN = "\033[32m";
    const char *RED = "\033[31m";
    const char *MAGENTA = "\033[35m";
    const char *WHITE = "\033[1;37m";
    const char *RESET = "\033[0m";
    const char *DIM = "\033[2m";

    /* Colores únicos por hilo */
    const char *HCOLORS[] = {
        "\033[41;37m", /* rojo */
        "\033[42;30m", /* verde */
        "\033[43;30m", /* amarillo */
        "\033[44;37m", /* azul */
        "\033[45;37m", /* magenta */
        "\033[46;30m", /* cyan */
        "\033[47;30m", /* blanco */
        "\033[100;37m" /* gris */
    };

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s     %s████████╗██╗███████╗███╗   ███╗██████╗  ██████╗%s                                 %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s   ██╔══╝██║██╔════╝████╗ ████║██╔══██╗██╔═══██╗%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s   ██║   ██║█████╗  ██╔████╔██║██████╔╝██║   ██║%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s   ██║   ██║██╔══╝  ██║╚██╔╝██║██╔═══╝ ██║   ██║%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s   ██║   ██║███████╗██║ ╚═╝ ██║██║     ╚██████╔╝%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s     %s   ╚═╝   ╚═╝╚══════╝╚═╝     ╚═╝╚═╝      ╚═════╝%s                                %s║%s\n", CYAN, RESET, YELLOW, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s     %sLÍNEA DE TIEMPO - RECURSOS ASIGNADOS A CADA HILO%s                                %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Calcular tiempos relativos */
    double t_min = 1e9, t_max = 0;
    double starts[8], ends[8];
    for (int i = 0; i < num_threads && i < 8; i++) {
        starts[i] = (args[i].ts_start.tv_sec - t0.tv_sec) +
                     (args[i].ts_start.tv_nsec - t0.tv_nsec) / 1e9;
        ends[i] = (args[i].ts_end.tv_sec - t0.tv_sec) +
                   (args[i].ts_end.tv_nsec - t0.tv_nsec) / 1e9;
        if (starts[i] < t_min) t_min = starts[i];
        if (ends[i] > t_max) t_max = ends[i];
    }

    double duration = t_max - t_min;
    if (duration <= 0) duration = elapsed;

    int BAR_WIDTH = 50;

    /* ─── Recurso 1: Tabla de recursos por hilo ─── */
    printf("%s║%s  %s┌─ RECURSOS ASIGNADOS A CADA HILO ────────────────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sHilo  TID         Stack Addr      Input HEAP (read)     Output HEAP (write)%s    │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  ────  ──────────  ──────────────  ────────────────────  ────────────────────    │  %s║%s\n",
           CYAN, RESET, CYAN, RESET);

    for (int i = 0; i < num_threads && i < 8; i++) {
        printf("%s║%s  │  %s[%d]%s   0x%-8lx  %s0x%012lx%s  0x%012lx %s→%s R  0x%012lx %s→%s W    │  %s║%s\n",
               CYAN, RESET, GREEN, i, RESET,
               (unsigned long)args[i].system_tid,
               MAGENTA, args[i].stack_addr ? (unsigned long)args[i].stack_addr : 0, RESET,
               (unsigned long)args[i].pixels,
               RED, RESET,
               (unsigned long)args[i].result.data,
               GREEN, RESET,
               CYAN, RESET);
    }

    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sR = Lectura compartida (sin mutex)   W = Escritura privada (sin mutex)%s        │  %s║%s\n",
           CYAN, RESET, DIM, RESET, CYAN, RESET);
    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* ─── Recurso 2: Línea de tiempo visual ─── */
    printf("%s║%s  %s┌─ LÍNEA DE TIEMPO DE EJECUCIÓN ────────────────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Escala de tiempo */
    printf("%s║%s  │  %sTiempo%s  0ms", CYAN, RESET, DIM, RESET);
    /* Marcas intermedias */
    for (int m = 1; m <= 4; m++) {
        int pos = (BAR_WIDTH * m) / 4;
        double t_val = duration * 1000.0 * m / 4;
        /* Padding hasta la posición */
        int current_pos = (m == 1) ? 4 : (BAR_WIDTH * (m-1)) / 4 + (m > 2 ? 7 : 6);
        for (int p = current_pos; p < pos + 4; p++) printf(" ");
        printf("%.1fms", t_val);
    }
    printf("    │  %s║%s\n", CYAN, RESET);

    printf("%s║%s  │          %s├", CYAN, RESET, DIM);
    for (int c = 0; c < BAR_WIDTH; c++) {
        if (c == BAR_WIDTH / 4 || c == BAR_WIDTH / 2 || c == 3 * BAR_WIDTH / 4)
            printf("┼");
        else
            printf("─");
    }
    printf("┤%s    │  %s║%s\n", RESET, CYAN, RESET);

    /* Barra del hilo padre */
    printf("%s║%s  │  %sPADRE%s   %s│%s", CYAN, RESET, MAGENTA, RESET, DIM, RESET);
    for (int c = 0; c < BAR_WIDTH; c++) {
        double t_pos = t_min + (duration * c) / BAR_WIDTH;
        /* El padre trabaja al inicio (create) y al final (join+write) */
        int creating = (t_pos < starts[num_threads > 1 ? num_threads - 1 : 0] + 0.001);
        int joining = (t_pos > ends[0] - 0.001);
        if (creating && !joining)
            printf("\033[45;37m░\033[0m"); /* creando hilos */
        else if (joining)
            printf("\033[45;37m▓\033[0m"); /* join + recoger */
        else
            printf("\033[2m·\033[0m"); /* bloqueado esperando */
    }
    printf("%s│%s    │  %s║%s\n", DIM, RESET, CYAN, RESET);

    /* Barra de cada hilo worker */
    for (int i = 0; i < num_threads && i < 8; i++) {
        printf("%s║%s  │  %s[H-%d]%s   %s│%s", CYAN, RESET, GREEN, i, RESET, DIM, RESET);
        for (int c = 0; c < BAR_WIDTH; c++) {
            double t_pos = t_min + (duration * c) / BAR_WIDTH;
            if (t_pos >= starts[i] && t_pos <= ends[i]) {
                printf("%s█%s", HCOLORS[i % 8], RESET);
            } else {
                printf("%s·%s", DIM, RESET);
            }
        }
        printf("%s│%s    │  %s║%s\n", DIM, RESET, CYAN, RESET);
    }

    /* Escala inferior */
    printf("%s║%s  │          %s├", CYAN, RESET, DIM);
    for (int c = 0; c < BAR_WIDTH; c++) {
        if (c == BAR_WIDTH / 4 || c == BAR_WIDTH / 2 || c == 3 * BAR_WIDTH / 4)
            printf("┼");
        else
            printf("─");
    }
    printf("┤%s    │  %s║%s\n", RESET, CYAN, RESET);

    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Leyenda */
    printf("%s║%s  │  %sLeyenda:%s                                                                        │  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │    %s░▓%s PADRE: ░=creando hilos  ▓=join/recoger  %s·%s=bloqueado (sin CPU)            │  %s║%s\n",
           CYAN, RESET, MAGENTA, RESET, DIM, RESET, CYAN, RESET);
    printf("%s║%s  │    %s██%s HIJOS: Ejecutando compresión RLE (cada color = 1 hilo)                    │  %s║%s\n",
           CYAN, RESET, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);

    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s                                                                                      %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* ─── Recurso 3: Detalle de tiempos por hilo ─── */
    printf("%s║%s  %s┌─ DETALLE DE RECURSOS Y TIEMPOS POR HILO ──────────────────────────────────────┐%s  %s║%s\n", CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sHilo  Inicio      Fin         Duración    CPU user    CPU sys     Datos%s       │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, CYAN, RESET);
    printf("%s║%s  │  ────  ──────────  ──────────  ──────────  ──────────  ──────────  ──────────   │  %s║%s\n",
           CYAN, RESET, CYAN, RESET);

    for (int i = 0; i < num_threads && i < 8; i++) {
        double dur_i = ends[i] - starts[i];
        printf("%s║%s  │  %s[%d]%s   %s%7.3f ms%s  %s%7.3f ms%s  %s%7.3f ms%s  %s%7.3f ms%s  %s%7.3f ms%s  %s%7zu B%s   │  %s║%s\n",
                CYAN, RESET, GREEN, i, RESET,
                YELLOW, starts[i] * 1000, RESET,
                YELLOW, ends[i] * 1000, RESET,
                GREEN, dur_i * 1000, RESET,
                MAGENTA, args[i].cpu_time_user * 1000, RESET,
                MAGENTA, args[i].cpu_time_sys * 1000, RESET,
                GREEN, args[i].num_pixels, RESET,
                CYAN, RESET);
    }

    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);

    /* Calcular máximo solapamiento: cuántos hilos corren a la vez */
    int max_concurrent = 0;
    double max_concurrent_time = 0;
    /* Muestrear 200 puntos en la línea de tiempo */
    for (int s = 0; s < 200; s++) {
        double t_sample = t_min + (duration * s) / 200.0;
        int concurrent = 0;
        for (int j = 0; j < num_threads && j < 8; j++) {
            if (t_sample >= starts[j] && t_sample <= ends[j])
                concurrent++;
        }
        if (concurrent > max_concurrent) {
            max_concurrent = concurrent;
            max_concurrent_time = t_sample;
        }
    }

    /* Calcular tiempo en que al menos 2 hilos corren simultáneamente */
    double overlap_time = 0;
    double step = duration / 1000.0;
    for (int s = 0; s < 1000; s++) {
        double t_sample = t_min + (duration * s) / 1000.0;
        int concurrent = 0;
        for (int j = 0; j < num_threads && j < 8; j++) {
            if (t_sample >= starts[j] && t_sample <= ends[j])
                concurrent++;
        }
        if (concurrent >= 2) overlap_time += step;
    }

    printf("%s║%s  │  %sMáximo hilos simultáneos:%s  %s%d hilos%s a los %s%.1f ms%s                              │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, RED, max_concurrent, RESET,
           GREEN, max_concurrent_time * 1000, RESET, CYAN, RESET);
    printf("%s║%s  │  %sTiempo con ≥2 hilos activos:%s  %s%.1f ms%s  (de %.1f ms total)                    │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, overlap_time * 1000, RESET, elapsed * 1000, CYAN, RESET);
    printf("%s║%s  │  %sDuración total (wall):%s  %s%.3f ms%s                                              │  %s║%s\n",
           CYAN, RESET, WHITE, RESET, GREEN, elapsed * 1000, RESET, CYAN, RESET);
    printf("%s║%s  │                                                                                  │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  └──────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN, RESET, CYAN, RESET);

    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN, RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  FUNCIÓN PRINCIPAL
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Variables en STACK - marcadores */
    int stack_marker_top = 0;
    Image img;
    int used_stb = 0;
    char input_path[512] = {0};

    /* Inicializar variable atómica global */
    atomic_init(&g_total_runs_atomic, 0);

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
        printf("\033[1;36m║\033[0m  \033[1;33mCOMPRESIÓN RLE - MODO PARALELO\033[0m                              \033[1;36m║\033[0m\n");
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

    /* Detectar número de cores */
    int num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads < 1) num_threads = 1;
    if ((size_t)num_threads > img.height) num_threads = (int)img.height;

    int stack_marker_bottom = 0;  /* Marcador base de pila */

    /* Preparar argumentos para cada hilo */
    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    ThreadArg *args = calloc(num_threads, sizeof(ThreadArg));
    if (!threads || !args) { perror("malloc"); return 1; }

    /* Distribuir filas equitativamente */
    uint32_t rows_per = img.height / num_threads;
    uint32_t extra = img.height % num_threads;
    uint32_t row_off = 0;

    for (int i = 0; i < num_threads; i++) {
        uint32_t rows = rows_per + (i < (int)extra ? 1 : 0);
        args[i].thread_idx = i;
        args[i].pixels = img.data + (size_t)row_off * img.width * 3;
        args[i].num_pixels = (size_t)rows * img.width * 3;
        args[i].start_row = row_off;
        args[i].num_rows = rows;
        args[i].byte_offset = (size_t)row_off * img.width * 3;
        atomic_init(&args[i].pixels_done, 0);
        args[i].system_tid = 0;
        args[i].stack_addr = NULL;
        args[i].cpu_time_user = 0;
        args[i].cpu_time_sys = 0;
        args[i].num_pc_samples = 0;
        args[i].t0_ref = NULL; /* Se asigna justo antes de crear hilos */
#ifdef __APPLE__
        args[i].mach_thread = 0;
        args[i].core_affinity = i % (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        row_off += rows;
    }

    /* Mostrar segmentos de memoria ANTES de crear los hilos */
    print_memory_segments(&img, args, num_threads, &stack_marker_top, &stack_marker_bottom);

    /* ═══════════════════════════════════════════════════════════════════════
     *  TRABAJO DEL HILO PADRE (main thread)
     * ═══════════════════════════════════════════════════════════════════════ */

    const char *CYAN_M = "\033[36m";
    const char *YELLOW_M = "\033[1;33m";
    const char *GREEN_M = "\033[32m";
    const char *RED_M = "\033[31m";
    const char *MAGENTA_M = "\033[35m";
    const char *WHITE_M = "\033[1;37m";
    const char *RESET_M = "\033[0m";

    /* Obtener TID y mach_port del hilo padre */
    uint64_t main_tid;
#ifdef __APPLE__
    pthread_threadid_np(NULL, &main_tid);
    mach_port_t main_mach_thread = pthread_mach_thread_np(pthread_self());
#else
    main_tid = (uint64_t)pthread_self();
#endif

    printf("\n");
    printf("%s╔══════════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN_M, RESET_M);
    printf("%s║%s     %s████████╗██████╗  █████╗ ██████╗  █████╗      ██╗ ██████╗%s                       %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %s   ██╔══╝██╔══██╗██╔══██╗██╔══██╗██╔══██╗     ██║██╔═══██╗%s                      %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %s   ██║   ██████╔╝███████║██████╔╝███████║     ██║██║   ██║%s                      %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %s   ██║   ██╔══██╗██╔══██║██╔══██╗██╔══██║██   ██║██║   ██║%s                      %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %s   ██║   ██║  ██║██║  ██║██████╔╝██║  ██║╚█████╔╝╚██████╔╝%s                      %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %s   ╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝╚═════╝ ╚═╝  ╚═╝ ╚════╝  ╚═════╝%s                      %s║%s\n", CYAN_M, RESET_M, MAGENTA_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s                                                                                      %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s     %sTRABAJO DEL HILO PADRE (main thread / thread de control)%s                        %s║%s\n", CYAN_M, RESET_M, WHITE_M, RESET_M, CYAN_M, RESET_M);
    printf("%s╠══════════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN_M, RESET_M);
    printf("%s║%s                                                                                      %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* Info del hilo padre */
    printf("%s║%s  %s┌─ IDENTIFICACIÓN DEL HILO PADRE ──────────────────────────────────────────────┐%s  %s║%s\n", CYAN_M, RESET_M, WHITE_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  PID del proceso:        %s%-10d%s                                            │  %s║%s\n",
           CYAN_M, RESET_M, GREEN_M, getpid(), RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  TID hilo padre:         %s0x%-10lx%s (es el hilo principal del proceso)      │  %s║%s\n",
           CYAN_M, RESET_M, GREEN_M, (unsigned long)main_tid, RESET_M, CYAN_M, RESET_M);
#ifdef __APPLE__
    printf("%s║%s  │  Mach port:              %s%-10u%s   (handle del kernel para este hilo)       │  %s║%s\n",
           CYAN_M, RESET_M, GREEN_M, main_mach_thread, RESET_M, CYAN_M, RESET_M);
#endif
    printf("%s║%s  │  Stack addr:             %s0x%014lx%s                                        │  %s║%s\n",
           CYAN_M, RESET_M, MAGENTA_M, (unsigned long)&stack_marker_top, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  Rol:                    %sCOORDINADOR%s  (no comprime, gestiona hilos)        │  %s║%s\n",
           CYAN_M, RESET_M, RED_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  └────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s                                                                                      %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* Línea de tiempo del trabajo del hilo padre */
    printf("%s║%s  %s┌─ LÍNEA DE TIEMPO DEL HILO PADRE ─────────────────────────────────────────────┐%s  %s║%s\n", CYAN_M, RESET_M, WHITE_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    struct timespec t_start, t_step, t_end;

    /* PASO 1: malloc/calloc */
    printf("%s║%s  │  %s[PASO 1]%s  malloc(pthread_t×%d) + calloc(ThreadArg×%d)                       │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, num_threads, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │            → threads:  %s0x%012lx%s  (%zu bytes en HEAP)                    │  %s║%s\n",
           CYAN_M, RESET_M, MAGENTA_M, (unsigned long)threads, RESET_M, sizeof(pthread_t) * num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │            → args:     %s0x%012lx%s  (%zu bytes en HEAP)                    │  %s║%s\n",
           CYAN_M, RESET_M, MAGENTA_M, (unsigned long)args, RESET_M, sizeof(ThreadArg) * num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 2: Distribuir trabajo */
    printf("%s║%s  │  %s[PASO 2]%s  Distribuir filas entre %d hilos (img.height / num_threads)          │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │            → %u filas/hilo, %u filas extra repartidas                          │  %s║%s\n",
           CYAN_M, RESET_M, rows_per, extra, CYAN_M, RESET_M);
    printf("%s║%s  │            → Cada hilo recibe puntero a su sección de img.data (read-only)     │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 3: Crear hilos */
    printf("%s║%s  │  %s[PASO 3]%s  %spthread_create()%s × %d  (el padre crea los hilos hijos)             │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, RED_M, RESET_M, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Pasar referencia de tiempo base a cada hilo */
    for (int i = 0; i < num_threads; i++)
        args[i].t0_ref = &t_start;

    /* Crear hilos de trabajo con log detallado */
    for (int i = 0; i < num_threads; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t_step);
        double t_rel = (t_step.tv_sec - t_start.tv_sec) +
                       (t_step.tv_nsec - t_start.tv_nsec) / 1e9;

        if (pthread_create(&threads[i], NULL, rle_thread_func, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }

#ifdef __APPLE__
        if (args[i].mach_thread) {
            thread_affinity_policy_data_t policy = { i + 1 };
            thread_policy_set(args[i].mach_thread,
                              THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy,
                              THREAD_AFFINITY_POLICY_COUNT);
        }
#endif

        printf("%s║%s  │    %s+%.4f ms%s  pthread_create(hilo[%d]) → TID %s0x%lx%s  afinidad=%s%d%s          │  %s║%s\n",
               CYAN_M, RESET_M, GREEN_M, t_rel * 1000, RESET_M, i,
               MAGENTA_M, (unsigned long)args[i].system_tid, RESET_M,
               YELLOW_M, i + 1, RESET_M, CYAN_M, RESET_M);
    }

    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 4: Esperar (join) */
    printf("%s║%s  │  %s[PASO 4]%s  %spthread_join()%s × %d  (el padre ESPERA a que terminen)              │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, RED_M, RESET_M, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │            → El hilo padre se %sBLOQUEA%s aquí hasta que todos terminen           │  %s║%s\n",
           CYAN_M, RESET_M, RED_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │            → Los hilos hijos están comprimiendo en PARALELO                    │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    for (int i = 0; i < num_threads; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t_step);
        double t_rel = (t_step.tv_sec - t_start.tv_sec) +
                       (t_step.tv_nsec - t_start.tv_nsec) / 1e9;

        pthread_join(threads[i], NULL);

        struct timespec t_after;
        clock_gettime(CLOCK_MONOTONIC, &t_after);
        double t_after_rel = (t_after.tv_sec - t_start.tv_sec) +
                             (t_after.tv_nsec - t_start.tv_nsec) / 1e9;

        printf("%s║%s  │    %s+%.4f ms%s  pthread_join(hilo[%d])  → completado en %s+%.4f ms%s             │  %s║%s\n",
               CYAN_M, RESET_M, GREEN_M, t_rel * 1000, RESET_M, i,
               GREEN_M, t_after_rel * 1000, RESET_M, CYAN_M, RESET_M);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 5: Recoger resultados */
    printf("%s║%s  │  %s[PASO 5]%s  Recoger resultados de cada hilo                                    │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, CYAN_M, RESET_M);

    /* Calcular métricas */
    double user_t, sys_t;
    get_process_cpu_times(&user_t, &sys_t);

    double total_thread_cpu = 0;
    size_t total_compressed = 0;
    for (int i = 0; i < num_threads; i++) {
        total_thread_cpu += args[i].cpu_time_user + args[i].cpu_time_sys;
        total_compressed += args[i].result.size;

        printf("%s║%s  │            hilo[%d]: result.data = %s0x%012lx%s  tamaño = %s%zu B%s               │  %s║%s\n",
               CYAN_M, RESET_M, i,
               MAGENTA_M, (unsigned long)args[i].result.data, RESET_M,
               GREEN_M, args[i].result.size, RESET_M, CYAN_M, RESET_M);
    }

    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 6: Escribir archivo */
    printf("%s║%s  │  %s[PASO 6]%s  fwrite() → concatenar buffers y escribir archivo de salida          │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │            → El padre escribe secuencialmente los %d buffers                    │  %s║%s\n",
           CYAN_M, RESET_M, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* PASO 7: free() */
    printf("%s║%s  │  %s[PASO 7]%s  free() × %d buffers + free(threads) + free(args) + free(img.data)  │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, RESET_M, num_threads, CYAN_M, RESET_M);
    printf("%s║%s  │            → El padre libera TODA la memoria dinámica                          │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    printf("%s║%s  └────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s                                                                                      %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);

    /* CPU time del hilo padre */
#ifdef __APPLE__
    thread_basic_info_data_t main_info;
    mach_msg_type_number_t main_count = THREAD_BASIC_INFO_COUNT;
    double main_user = 0, main_sys = 0;
    if (thread_info(main_mach_thread, THREAD_BASIC_INFO,
                    (thread_info_t)&main_info, &main_count) == KERN_SUCCESS) {
        main_user = main_info.user_time.seconds + main_info.user_time.microseconds / 1e6;
        main_sys = main_info.system_time.seconds + main_info.system_time.microseconds / 1e6;
    }
    printf("%s║%s  %s┌─ CPU TIME DEL HILO PADRE (via Mach thread_info) ──────────────────────────────┐%s  %s║%s\n", CYAN_M, RESET_M, WHITE_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  user_time:    %s%10.4f ms%s                                                    │  %s║%s\n",
           CYAN_M, RESET_M, GREEN_M, main_user * 1000, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  sys_time:     %s%10.4f ms%s                                                    │  %s║%s\n",
           CYAN_M, RESET_M, GREEN_M, main_sys * 1000, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  total:        %s%10.4f ms%s  (overhead de coordinación)                        │  %s║%s\n",
           CYAN_M, RESET_M, YELLOW_M, (main_user + main_sys) * 1000, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │                                                                                │  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │  %sNota: El hilo padre NO comprime datos. Su CPU se gasta en:%s                   │  %s║%s\n",
           CYAN_M, RESET_M, WHITE_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │    • pthread_create() - crear stacks y registrar hilos en el SO               │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │    • thread_policy_set() - configurar afinidad de CPU                         │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │    • pthread_join() - esperar (bloqueado, casi 0 CPU)                         │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │    • fwrite() - escribir resultado a disco                                    │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  │    • malloc/free - gestionar memoria dinámica                                 │  %s║%s\n",
           CYAN_M, RESET_M, CYAN_M, RESET_M);
    printf("%s║%s  └────────────────────────────────────────────────────────────────────────────────┘  %s║%s\n", CYAN_M, RESET_M, CYAN_M, RESET_M);
#endif

    printf("%s╚══════════════════════════════════════════════════════════════════════════════════════╝%s\n", CYAN_M, RESET_M);

    /* Mostrar segmentos de memoria DESPUÉS de ejecutar (actualizado) */
    print_memory_segments(&img, args, num_threads, &stack_marker_top, &stack_marker_bottom);

    /* Mostrar resultados de ejecución */
    const char *CYAN = "\033[36m";
    const char *RESET = "\033[0m";

    printf("\n");
    printf("%s╔════════════════════════════════════════════════════════════════════════════════╗%s\n", CYAN, RESET);
    printf("%s║%s              \033[33m*** RESULTADOS DE EJECUCIÓN - MODO PARALELO ***\033[0m               %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s╠════════════════════════════════════════════════════════════════════════════════╣%s\n", CYAN, RESET);

    print_thread_distribution(args, num_threads, &img, "COMPLETADO");
    print_thread_results(args, num_threads);
    print_execution_metrics(elapsed, user_t, sys_t, total_thread_cpu,
                            total_compressed + 8, raw_size, num_threads);

    /* Mostrar línea de tiempo de recursos */
    print_resource_timeline(args, num_threads, t_start, elapsed);

    /* Escribir archivo de salida */
    char outpath[512];
    if (input_path[0])
        snprintf(outpath, sizeof(outpath), "%s_paralelo.rle", input_path);
    else
        snprintf(outpath, sizeof(outpath), "output_paralelo.rle");

    FILE *fout = fopen(outpath, "wb");
    size_t total_compressed_size = 0;
    if (fout) {
        uint32_t header[2] = { img.width, img.height };
        fwrite(header, sizeof(uint32_t), 2, fout);
        for (int i = 0; i < num_threads; i++) {
            fwrite(args[i].result.data, 1, args[i].result.size, fout);
            total_compressed_size += args[i].result.size;
        }
        fclose(fout);
        printf("\n  \033[32mArchivo comprimido guardado:\033[0m %s\n", outpath);
    }

    /* ═══════════════════════════════════════════════════════════════════
     *  DESCOMPRESIÓN Y GENERACIÓN DE IMAGEN BMP
     * ═══════════════════════════════════════════════════════════════════ */
    printf("\n\033[33m  Descomprimiendo datos RLE...\033[0m\n");

    /* Concatenar todos los buffers de los hilos en uno solo */
    uint8_t *all_rle = (uint8_t *)malloc(total_compressed_size);
    if (all_rle) {
        size_t off = 0;
        for (int i = 0; i < num_threads; i++) {
            memcpy(all_rle + off, args[i].result.data, args[i].result.size);
            off += args[i].result.size;
        }

        struct timespec td_start, td_end;
        clock_gettime(CLOCK_MONOTONIC, &td_start);

        uint8_t *decoded = rle_decompress(all_rle, total_compressed_size, total_pixels * 3);

        clock_gettime(CLOCK_MONOTONIC, &td_end);
        double decomp_time = (td_end.tv_sec - td_start.tv_sec) +
                             (td_end.tv_nsec - td_start.tv_nsec) / 1e9;

        if (decoded) {
            int match = (memcmp(decoded, img.data, raw_size) == 0);

            char bmppath[512];
            if (input_path[0])
                snprintf(bmppath, sizeof(bmppath), "%s_paralelo_descomprimida.bmp", input_path);
            else
                snprintf(bmppath, sizeof(bmppath), "output_paralelo_descomprimida.bmp");

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
        free(all_rle);
    }

    /* ═══════════════════════════════════════════════════════════════════
     *  EXPORTAR CSV CON DATOS DE SCHEDULING PARA DIAGRAMA DE GANTT
     * ═══════════════════════════════════════════════════════════════════ */
    {
        char csvpath[512];
        if (input_path[0])
            snprintf(csvpath, sizeof(csvpath), "%s_paralelo_gantt.csv", input_path);
        else
            snprintf(csvpath, sizeof(csvpath), "output_paralelo_gantt.csv");

        FILE *csv = fopen(csvpath, "w");
        if (csv) {
            fprintf(csv, "algorithm,pid,num_threads,wall_ms,total_cpu_ms\n");
            fprintf(csv, "paralelo,%d,%d,%.4f,%.4f\n",
                    getpid(), num_threads, elapsed * 1000,
                    (user_t + sys_t) * 1000);
            fprintf(csv, "\n");

            /* Datos por hilo */
            fprintf(csv, "thread_id,tid,core,start_ms,end_ms,cpu_user_ms,cpu_sys_ms,pixels,compressed_bytes,stack_addr\n");
            for (int i = 0; i < num_threads; i++) {
                double st = ts_relative_ms(&t_start, &args[i].ts_start);
                double en = ts_relative_ms(&t_start, &args[i].ts_end);
                fprintf(csv, "%d,0x%lx,%d,%.4f,%.4f,%.4f,%.4f,%zu,%zu,0x%lx\n",
                        i, (unsigned long)args[i].system_tid,
                        args[i].core_affinity,
                        st, en,
                        args[i].cpu_time_user * 1000,
                        args[i].cpu_time_sys * 1000,
                        args[i].num_pixels,
                        args[i].result.size,
                        (unsigned long)args[i].stack_addr);
            }
            fprintf(csv, "\n");

            /* Muestras del PC (Program Counter) por hilo */
            fprintf(csv, "thread_id,sample_idx,timestamp_ms,pc_addr,core_id,pixels_at\n");
            for (int i = 0; i < num_threads; i++) {
                for (int s = 0; s < args[i].num_pc_samples; s++) {
                    PCSample *ps = &args[i].pc_samples[s];
                    fprintf(csv, "%d,%d,%.4f,0x%lx,%d,%zu\n",
                            i, s, ps->timestamp_ms,
                            (unsigned long)ps->pc_addr,
                            ps->core_id, ps->pixels_at);
                }
            }

            fclose(csv);
            printf("  \033[32mDatos de scheduling exportados:\033[0m %s\n\n", csvpath);
        }
    }

    /* Liberar memoria */
    if (used_stb)
        stbi_image_free(img.data);
    else
        free(img.data);
    for (int i = 0; i < num_threads; i++)
        free(args[i].result.data);
    free(threads);
    free(args);
    return 0;
}
