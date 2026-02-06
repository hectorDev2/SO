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

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <sys/sysctl.h>
#endif

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

#ifdef __APPLE__
    mach_port_t mach_thread;
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
    img->data = malloc(pixel_bytes);
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
    img->data = malloc(pixel_bytes);
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
 *  FUNCIÓN DEL HILO DE COMPRESIÓN
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *rle_thread_func(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;

    /* Capturar información del hilo */
    int stack_var = 0;  /* Variable local para obtener dirección del stack */
    ta->stack_addr = &stack_var;

#ifdef __APPLE__
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    ta->system_tid = tid;
    ta->mach_thread = pthread_mach_thread_np(pthread_self());
#else
    ta->system_tid = (uint64_t)pthread_self();
#endif

    /* Inicializar buffer de salida */
    buffer_init(&ta->result, ta->num_pixels * 3 / 2 + 256);

    /* Compresión RLE */
    const uint8_t *pixels = ta->pixels;
    size_t num_pixels = ta->num_pixels;
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
        buffer_push(&ta->result, run, 4);
        i += count;
        atomic_store(&ta->pixels_done, i);
        atomic_fetch_add(&g_total_runs_atomic, 1);  /* Contador global atómico */
    }

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
static int load_ppm(const char *path, Image *img);

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
    printf("%s║%s  %s▓%s  │  load_ppm()             %s0x%014lx%s  Cargar PPM                 │ %s▓%s  %s║%s\n",
           CYAN, RESET, YELLOW, RESET, MAGENTA, (unsigned long)load_ppm, RESET, YELLOW, RESET, CYAN, RESET);
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
    printf("%s║%s  %s▓%s  │  img.data               %s0x%014lx%s  %10zu bytes           │ %s▓%s  %s║%s\n",
           CYAN, RESET, GREEN, RESET, MAGENTA, (unsigned long)img->data, RESET,
           (size_t)img->width * img->height * 3, GREEN, RESET, CYAN, RESET);
    printf("%s║%s  %s▓%s  │  Imagen:                %u x %u píxeles (RGB)                         │ %s▓%s  %s║%s\n",
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
               GREEN, ta->num_pixels * 3, RESET,
               CYAN, RESET);
    }

    printf("%s║%s  │                                                                          │  %s║%s\n", CYAN, RESET, CYAN, RESET);
    printf("%s║%s  │  %sTotal: %u filas, %zu píxeles, %zu bytes de entrada%s                     │  %s║%s\n",
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
 *  FUNCIÓN PRINCIPAL
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Variables en STACK - marcadores */
    int stack_marker_top = 0;
    Image img;

    /* Inicializar variable atómica global */
    atomic_init(&g_total_runs_atomic, 0);

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
        args[i].num_pixels = (size_t)rows * img.width;
        args[i].start_row = row_off;
        args[i].num_rows = rows;
        args[i].byte_offset = (size_t)row_off * img.width * 3;
        atomic_init(&args[i].pixels_done, 0);
        args[i].system_tid = 0;
        args[i].stack_addr = NULL;
        args[i].cpu_time_user = 0;
        args[i].cpu_time_sys = 0;
#ifdef __APPLE__
        args[i].mach_thread = 0;
#endif
        row_off += rows;
    }

    /* Mostrar segmentos de memoria ANTES de crear los hilos */
    print_memory_segments(&img, args, num_threads, &stack_marker_top, &stack_marker_bottom);

    printf("\n\033[33m  Creando %d hilos de trabajo...\033[0m\n", num_threads);

    /* Medir tiempo */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Crear hilos de trabajo */
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, rle_thread_func, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }

#ifdef __APPLE__
        /* Esperar a que el hilo registre su info */
        usleep(1000);
        if (args[i].mach_thread) {
            /* Configurar afinidad: tags diferentes -> cores diferentes */
            thread_affinity_policy_data_t policy = { i + 1 };
            thread_policy_set(args[i].mach_thread,
                              THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy,
                              THREAD_AFFINITY_POLICY_COUNT);
        }
#endif
    }

    /* Esperar a que todos terminen */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* Calcular métricas */
    double user_t, sys_t;
    get_process_cpu_times(&user_t, &sys_t);

    double total_thread_cpu = 0;
    size_t total_compressed = 0;
    for (int i = 0; i < num_threads; i++) {
        total_thread_cpu += args[i].cpu_time_user + args[i].cpu_time_sys;
        total_compressed += args[i].result.size;
    }

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

    /* Escribir archivo de salida */
    char outpath[512];
    if (argc >= 2)
        snprintf(outpath, sizeof(outpath), "%s.rle", argv[1]);
    else
        snprintf(outpath, sizeof(outpath), "output_paralelo.rle");

    FILE *fout = fopen(outpath, "wb");
    if (fout) {
        uint32_t header[2] = { img.width, img.height };
        fwrite(header, sizeof(uint32_t), 2, fout);
        for (int i = 0; i < num_threads; i++)
            fwrite(args[i].result.data, 1, args[i].result.size, fout);
        fclose(fout);
        printf("\n  Archivo guardado: %s\n\n", outpath);
    }

    /* Liberar memoria */
    free(img.data);
    for (int i = 0; i < num_threads; i++)
        free(args[i].result.data);
    free(threads);
    free(args);
    return 0;
}
