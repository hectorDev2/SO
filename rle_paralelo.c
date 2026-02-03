/*
 * ============================================================================
 *  rle_paralelo.c — Compresión RLE de Imágenes (Versión Paralela con Pthreads)
 * ============================================================================
 *
 *  DESCRIPCIÓN:
 *      Implementa el algoritmo de compresión Run-Length Encoding (RLE) sobre
 *      imágenes RGB utilizando MÚLTIPLES HILOS de ejecución (uno por core
 *      disponible) mediante la biblioteca POSIX Threads (pthreads).
 *
 *  ESTRATEGIA DE PARALELIZACIÓN:
 *      La imagen se divide horizontalmente en N bloques de filas, donde N es
 *      el número de cores del procesador (detectados con sysconf). Cada hilo
 *      comprime su bloque de forma completamente independiente en un buffer
 *      local, eliminando la necesidad de sincronización durante el cómputo.
 *
 *      ┌─────────────────────┐
 *      │  Bloque 0 (Hilo 0) │ ← filas 0 a 511
 *      ├─────────────────────┤
 *      │  Bloque 1 (Hilo 1) │ ← filas 512 a 1023
 *      ├─────────────────────┤
 *      │  Bloque 2 (Hilo 2) │ ← filas 1024 a 1535
 *      ├─────────────────────┤
 *      │       ...           │
 *      ├─────────────────────┤
 *      │  Bloque N (Hilo N) │ ← filas restantes
 *      └─────────────────────┘
 *
 *      Después del join, el hilo principal concatena los buffers comprimidos
 *      en orden para producir el archivo final.
 *
 *  AFINIDAD DE CORES (macOS):
 *      En macOS, se usa thread_policy_set con THREAD_AFFINITY_POLICY para
 *      sugerir al scheduler que distribuya los hilos en cores diferentes.
 *      Cada hilo recibe un tag de afinidad único, indicando al kernel que
 *      hilos con tags diferentes deberían ejecutarse en cores separados.
 *
 *  MÉTRICAS POR HILO:
 *      - TID del sistema (thread ID nativo del OS)
 *      - Core asignado (índice lógico)
 *      - Progreso individual (barra visual)
 *      - Tiempo de CPU consumido (vía Mach thread_info en macOS)
 *      - Filas y píxeles asignados
 *      - Bytes comprimidos producidos
 *
 *  COMPILACIÓN:
 *      gcc -Wall -Wextra -O2 -o rle_paralelo rle_paralelo.c -lpthread
 *
 *  USO:
 *      ./rle_paralelo                    # imagen sintética 4096×4096
 *      ./rle_paralelo imagen.ppm         # archivo PPM real
 *
 *  AUTOR:   Proyecto de Sistemas Operativos
 *  FECHA:   2025
 *  LICENCIA: Uso educativo
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>           /* clock_gettime, CLOCK_MONOTONIC */
#include <sys/resource.h>   /* getrusage — tiempos CPU del proceso */
#include <unistd.h>         /* usleep, sysconf */
#include <stdatomic.h>      /* atomic_size_t — progreso sin locks */
#include <pthread.h>        /* pthread_create/join — paralelismo */

#ifdef __APPLE__
#include <mach/mach.h>           /* task_info — memoria RSS */
#include <mach/thread_policy.h>  /* thread_policy_set — afinidad de cores */
#include <mach/thread_act.h>     /* thread_info — CPU por hilo */
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 1: ESTRUCTURAS DE DATOS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Image: Imagen RGB en memoria (ver documentación en rle_secuencial.c).
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *data;
} Image;

/*
 * Buffer: Buffer dinámico con crecimiento amortizado.
 */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} Buffer;

/*
 * ThreadArg: Argumentos y estado para cada hilo de compresión.
 *
 * Cada hilo recibe un puntero a su porción de la imagen y un buffer
 * local donde escribe los datos comprimidos. El campo pixels_done es
 * una variable atómica que permite al hilo monitor leer el progreso
 * sin necesidad de mutex (patrón productor-consumidor lock-free).
 *
 * En macOS, mach_thread almacena el port Mach del hilo, necesario
 * para consultar su consumo de CPU individual mediante thread_info().
 */
typedef struct {
    /* Identificación */
    int thread_idx;            /* Índice del hilo (0 a N-1) */
    uint64_t system_tid;       /* TID asignado por el sistema operativo */

    /* Datos de entrada (solo lectura, no requiere sincronización) */
    const uint8_t *pixels;     /* Puntero al inicio del bloque en la imagen */
    size_t num_pixels;         /* Número de píxeles en este bloque */
    uint32_t start_row;        /* Primera fila asignada */
    uint32_t num_rows;         /* Número de filas asignadas */

    /* Datos de salida (escritura exclusiva por este hilo) */
    Buffer result;             /* Buffer con datos RLE comprimidos */

    /* Progreso (escritura por hilo, lectura por monitor) */
    atomic_size_t pixels_done; /* Píxeles procesados hasta ahora */

    /* Métricas (escritura al finalizar) */
    double cpu_time;           /* Tiempo CPU total consumido por este hilo */

#ifdef __APPLE__
    mach_port_t mach_thread;   /* Port Mach para consultar CPU por hilo */
#endif
} ThreadArg;

/*
 * MonitorState: Estado global para el hilo monitor de visualización.
 *
 * Contiene referencias a todos los ThreadArgs para poder consultar
 * el progreso de cada hilo, más metadata de la imagen para calcular
 * porcentajes y throughput globales.
 */
typedef struct {
    ThreadArg *args;           /* Array de argumentos de todos los hilos */
    int num_threads;           /* Número total de hilos de compresión */
    size_t total_pixels;       /* Total de píxeles en la imagen */
    size_t raw_size;           /* Tamaño original en bytes */
    struct timespec *t_start;  /* Inicio del cronómetro */
    atomic_int done;           /* Flag: 1 cuando todos los hilos terminaron */
} MonitorState;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 2: BUFFER DINÁMICO
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
 *  SECCIÓN 3: MÉTRICAS DEL SISTEMA
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * get_rss: Memoria física (RSS) del proceso completo.
 * (Ver documentación detallada en rle_secuencial.c)
 */
static size_t get_rss(void) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages;
    if (fscanf(f, "%*d %ld", &pages) != 1) { fclose(f); return 0; }
    fclose(f);
    return (size_t)pages * sysconf(_SC_PAGESIZE);
#endif
}

/*
 * get_thread_cpu: Obtiene el tiempo de CPU consumido por un hilo específico.
 *
 * @param ta  Argumento del hilo (contiene el mach port)
 * @return    Segundos de CPU (usuario + sistema) consumidos por el hilo
 *
 * En macOS: usa Mach API thread_info(THREAD_BASIC_INFO) que retorna
 * tiempos de usuario y sistema desglosados por hilo individual.
 *
 * Esto es crucial para demostrar que MÚLTIPLES hilos están consumiendo
 * CPU simultáneamente (paralelismo real, no concurrencia simulada).
 * Si la suma de CPU de todos los hilos > wall time, hay paralelismo real.
 */
static double get_thread_cpu(ThreadArg *ta) {
#ifdef __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (ta->mach_thread &&
        thread_info(ta->mach_thread, THREAD_BASIC_INFO,
                    (thread_info_t)&info, &count) == KERN_SUCCESS) {
        return info.user_time.seconds + info.user_time.microseconds / 1e6 +
               info.system_time.seconds + info.system_time.microseconds / 1e6;
    }
    return 0;
#else
    (void)ta;
    return 0; /* En Linux se podría usar clock_gettime(CLOCK_THREAD_CPUTIME_ID) */
#endif
}

/*
 * get_cpu_times: Tiempos CPU del proceso completo (todos los hilos sumados).
 */
static void get_cpu_times(double *user_s, double *sys_s) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *user_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
    *sys_s  = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 4: LECTURA DE ARCHIVOS PPM
 * ═══════════════════════════════════════════════════════════════════════════ */

/* (Ver documentación detallada del formato PPM en rle_secuencial.c) */
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 5: GENERACIÓN DE IMAGEN SINTÉTICA
 * ═══════════════════════════════════════════════════════════════════════════ */

/* (Ver documentación detallada de la generación en rle_secuencial.c) */
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
 *  SECCIÓN 6: VISUALIZACIÓN EN TIEMPO REAL (PARALELA)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BAR_WIDTH 25

/*
 * print_bar: Barra de progreso con caracteres Unicode y colores ANSI.
 * (Ver documentación en rle_secuencial.c)
 */
static void print_bar(double pct) {
    int filled = (int)(pct / 100.0 * BAR_WIDTH);
    if (filled > BAR_WIDTH) filled = BAR_WIDTH;
    printf("\033[32m");
    for (int i = 0; i < filled; i++) printf("█");
    printf("\033[90m");
    for (int i = filled; i < BAR_WIDTH; i++) printf("░");
    printf("\033[0m");
}

/*
 * print_display: Dibuja el panel completo de métricas para la versión paralela.
 *
 * @param ms  Estado del monitor con acceso a todos los hilos
 * @return    Número de líneas impresas (para mover cursor en la próxima iteración)
 *
 * El display muestra una TABLA donde cada fila corresponde a un hilo:
 *
 *   Hilo  TID         Core  Progreso                        CPU(ms)  Filas
 *   ───── ─────────── ────  ─────────────────────────────── ──────── ─────
 *    0    0x1A3F        0   [████████████████████████████] 100%  2.1    512
 *    1    0x1A40        1   [██████████████████████░░░░░░]  78%  1.8    512
 *    ...
 *
 * Esto permite observar visualmente:
 *   - Todos los hilos progresando SIMULTÁNEAMENTE (paralelismo real)
 *   - Distribución equitativa del trabajo (filas similares por hilo)
 *   - Cada hilo consumiendo CPU de forma independiente
 *   - Uso de CPU > 100% (indica múltiples cores activos)
 */
static int print_display(MonitorState *ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - ms->t_start->tv_sec) +
                     (now.tv_nsec - ms->t_start->tv_nsec) / 1e9;

    int lines = 0;

    /* ─── Header del recuadro ─── */
    printf("\033[36m╔═══════════════════════════════════════════════════════════════════════════════╗\033[0m\n"); lines++;
    printf("\033[36m║\033[0m   \033[1;33mCOMPRESIÓN RLE — MODO PARALELO (%d hilos)\033[0m", ms->num_threads);
    int title_len = 37 + (ms->num_threads >= 10 ? 2 : 1);
    for (int i = title_len; i < 76; i++) printf(" ");
    printf("\033[36m║\033[0m\n"); lines++;
    printf("\033[36m╠═══════════════════════════════════════════════════════════════════════════════╣\033[0m\n"); lines++;
    printf("\033[36m║\033[0m  Datos originales: \033[1m%zu\033[0m bytes (%zu píxeles)",
           ms->raw_size, ms->total_pixels);
    printf("                          \033[36m║\033[0m\n"); lines++;
    printf("\033[36m╠═══════════════════════════════════════════════════════════════════════════════╣\033[0m\n"); lines++;

    /* ─── Cabecera de la tabla de hilos ─── */
    printf("\033[36m║\033[0m \033[1;37m Hilo  TID         Core  Progreso                        CPU(ms)  Filas \033[0m\033[36m║\033[0m\n"); lines++;
    printf("\033[36m║\033[0m ───── ─────────── ────  ─────────────────────────────── ──────── ───── \033[36m║\033[0m\n"); lines++;

    /* ─── Fila por cada hilo de compresión ─── */
    size_t total_done = 0;
    size_t total_comp = 0;

    for (int i = 0; i < ms->num_threads; i++) {
        ThreadArg *ta = &ms->args[i];
        size_t done = atomic_load(&ta->pixels_done);
        total_done += done;
        total_comp += ta->result.size;

        /* Calcular progreso individual */
        double pct = ta->num_pixels > 0
                     ? (double)done / ta->num_pixels * 100.0 : 0;
        if (pct > 100.0) pct = 100.0;

        /* Obtener CPU individual de este hilo */
        double cpu_ms = get_thread_cpu(ta) * 1000.0;

        printf("\033[36m║\033[0m  %2d   0x%-8lx   %2d   ",
               i, (unsigned long)ta->system_tid, i);
        print_bar(pct);
        printf(" %5.1f%%  %6.1f  %5u \033[36m║\033[0m\n",
               pct, cpu_ms, ta->num_rows);
        lines++;
    }

    /* ─── Métricas globales ─── */
    printf("\033[36m╠═══════════════════════════════════════════════════════════════════════════════╣\033[0m\n"); lines++;

    double global_pct = ms->total_pixels > 0
                        ? (double)total_done / ms->total_pixels * 100.0 : 0;
    if (global_pct > 100.0) global_pct = 100.0;

    double user_t, sys_t;
    get_cpu_times(&user_t, &sys_t);
    double cpu_total = user_t + sys_t;

    /*
     * Uso de CPU > 100% es la PRUEBA de paralelismo real.
     * Si hay N cores activos, el uso teórico máximo es N × 100%.
     * Ejemplo: 8 hilos en 8 cores → hasta 800% de uso de CPU.
     */
    double cpu_pct = elapsed > 0.001 ? cpu_total / elapsed * 100.0 : 0;
    size_t rss = get_rss();
    double throughput = elapsed > 0.001
                        ? (total_done * 3.0 / (1024.0 * 1024.0)) / elapsed : 0;

    printf("\033[36m║\033[0m  Progreso global: ");
    print_bar(global_pct);
    printf(" \033[1m%5.1f%%\033[0m                      \033[36m║\033[0m\n", global_pct); lines++;
    printf("\033[36m║\033[0m                                                                             \033[36m║\033[0m\n"); lines++;
    printf("\033[36m║\033[0m  Tiempo transcurrido:  \033[1m%8.6f\033[0m s        Uso CPU: \033[1m%6.1f%%\033[0m               \033[36m║\033[0m\n",
           elapsed, cpu_pct); lines++;
    printf("\033[36m║\033[0m  CPU total (usr+sys):  \033[1m%8.6f\033[0m s        Memoria: \033[1m%5.1f\033[0m MB               \033[36m║\033[0m\n",
           cpu_total, rss / (1024.0 * 1024.0)); lines++;
    printf("\033[36m║\033[0m  Throughput:           \033[1m%7.1f\033[0m MB/s      Comprimido: \033[1m%zu\033[0m bytes       \033[36m║\033[0m\n",
           throughput, total_comp); lines++;
    printf("\033[36m╚═══════════════════════════════════════════════════════════════════════════════╝\033[0m\n"); lines++;

    return lines;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 7: HILO MONITOR
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * monitor_func: Hilo auxiliar que refresca la visualización cada 80ms.
 *
 * Patrón de actualización en terminal:
 *   1. Imprimir el display completo (N líneas)
 *   2. Esperar 80ms
 *   3. Mover cursor arriba N líneas con \033[NA
 *   4. Sobreescribir con datos actualizados
 *
 * El número de líneas varía con el número de hilos, por lo que
 * print_display() retorna el conteo para el cursor.
 */
static void *monitor_func(void *arg) {
    MonitorState *ms = (MonitorState *)arg;
    int prev_lines = 0;

    while (!atomic_load(&ms->done)) {
        if (prev_lines > 0)
            printf("\033[%dA", prev_lines);
        prev_lines = print_display(ms);
        fflush(stdout);
        usleep(80000);
    }

    /* Refresco final con datos completos */
    if (prev_lines > 0) printf("\033[%dA", prev_lines);
    print_display(ms);
    fflush(stdout);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 8: HILO DE COMPRESIÓN RLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * rle_thread_func: Función ejecutada por cada hilo de compresión.
 *
 * @param arg  Puntero a ThreadArg con los datos del bloque asignado
 * @return     NULL (resultado en ta->result)
 *
 * Flujo de ejecución:
 *   1. Registrar TID del sistema y mach port (para métricas)
 *   2. Inicializar buffer de salida local
 *   3. Ejecutar RLE sobre el bloque de píxeles asignado
 *   4. Actualizar progreso atómico después de cada run
 *   5. Registrar tiempo CPU final
 *
 * INDEPENDENCIA: Cada hilo trabaja sobre una porción DISJUNTA de la
 * imagen, con su propio buffer de salida. No hay escrituras compartidas
 * durante la compresión, lo que elimina:
 *   - Condiciones de carrera
 *   - Necesidad de mutex o semáforos
 *   - False sharing (los buffers están en distintas regiones de heap)
 *
 * La única sincronización es la variable atómica pixels_done que se
 * actualiza al final de cada run (operación lock-free de ~1 nanosegundo).
 */
static void *rle_thread_func(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;

    /* ─── Registrar identificadores del hilo ─── */
#ifdef __APPLE__
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    ta->system_tid = tid;
    ta->mach_thread = pthread_mach_thread_np(pthread_self());
#else
    ta->system_tid = (uint64_t)pthread_self();
#endif

    /* ─── Inicializar buffer de salida local ─── */
    buffer_init(&ta->result, ta->num_pixels * 3 / 2 + 256);

    const uint8_t *pixels = ta->pixels;
    size_t num_pixels = ta->num_pixels;
    size_t i = 0;

    /* ─── Algoritmo RLE (idéntico al secuencial) ─── */
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

        /* Actualizar progreso atómico (lock-free) */
        atomic_store(&ta->pixels_done, i);
    }

    /* ─── Registrar CPU time al finalizar ─── */
    ta->cpu_time = get_thread_cpu(ta);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 9: FUNCIÓN PRINCIPAL
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    Image img;

    /* ─── Cargar o generar imagen ─── */
    if (argc >= 2) {
        printf("Cargando imagen PPM: %s\n", argv[1]);
        if (load_ppm(argv[1], &img) != 0) {
            fprintf(stderr, "Error: no se pudo leer '%s' como PPM P6\n", argv[1]);
            return 1;
        }
    } else {
        printf("Generando imagen sintética 4096x4096...\n");
        generate_synthetic(&img, 4096, 4096);
    }

    size_t total_pixels = (size_t)img.width * img.height;
    size_t raw_size = total_pixels * 3;

    /* ─── Detectar número de cores disponibles ─── */
    /*
     * sysconf(_SC_NPROCESSORS_ONLN) retorna el número de procesadores
     * lógicos actualmente disponibles (online). En un CPU con
     * hyperthreading, esto incluye los hilos lógicos de cada core.
     *
     * Creamos exactamente un hilo por core para maximizar el paralelismo
     * sin overhead de context switching excesivo.
     */
    int num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads < 1) num_threads = 1;
    if ((size_t)num_threads > img.height) num_threads = (int)img.height;

    char outpath[512];
    if (argc >= 2)
        snprintf(outpath, sizeof(outpath), "%s.rle", argv[1]);
    else
        snprintf(outpath, sizeof(outpath), "output_paralelo.rle");

    /* ─── Preparar argumentos de cada hilo ─── */
    /*
     * División del trabajo: las filas se reparten equitativamente.
     * Si height no es divisible por num_threads, los primeros hilos
     * reciben una fila extra (distribución round-robin del residuo).
     *
     * Ejemplo con height=4096, threads=8:
     *   Cada hilo: 512 filas × 4096 cols = 2,097,152 píxeles
     *   Memoria por bloque de entrada: ~6 MB (read-only, sin copia)
     */
    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    ThreadArg *args = calloc(num_threads, sizeof(ThreadArg));
    if (!threads || !args) { perror("malloc"); return 1; }

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
        atomic_init(&args[i].pixels_done, 0);
        args[i].cpu_time = 0;
        args[i].system_tid = 0;
#ifdef __APPLE__
        args[i].mach_thread = 0;
#endif
        row_off += rows;
    }

    /* ─── Configurar estado del monitor ─── */
    MonitorState ms;
    ms.args = args;
    ms.num_threads = num_threads;
    ms.total_pixels = total_pixels;
    ms.raw_size = raw_size;
    atomic_init(&ms.done, 0);

    printf("\n");

    /* ─── Iniciar cronómetro ─── */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    ms.t_start = &t_start;

    /* ─── Lanzar hilo monitor ─── */
    pthread_t mon_tid;
    pthread_create(&mon_tid, NULL, monitor_func, &ms);

    /* ─── Crear hilos de compresión ─── */
    /*
     * Cada hilo se crea con pthread_create y se le aplica una política
     * de afinidad (en macOS) para sugerir al scheduler que lo ejecute
     * en un core diferente.
     *
     * THREAD_AFFINITY_POLICY con tags diferentes indica al kernel macOS
     * que los hilos deberían estar en cores DISTINTOS. El kernel trata
     * esto como una "sugerencia" (hint), no como un mandato.
     */
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, rle_thread_func, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }

#ifdef __APPLE__
        /* Esperar brevemente a que el hilo registre su mach_thread */
        usleep(100);
        if (args[i].mach_thread) {
            /*
             * Tag de afinidad: hilos con tags diferentes serán
             * programados en cores diferentes por el scheduler.
             * Tag 0 se reserva ("no preference"), usamos i+1.
             */
            thread_affinity_policy_data_t policy = { i + 1 };
            thread_policy_set(args[i].mach_thread,
                              THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy,
                              THREAD_AFFINITY_POLICY_COUNT);
        }
#endif
    }

    /* ─── Esperar a que todos los hilos terminen (barrier implícito) ─── */
    /*
     * pthread_join bloquea hasta que el hilo objetivo termine.
     * Al hacer join de todos los hilos, garantizamos que toda la
     * compresión terminó antes de comenzar la fase de merge.
     */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* ─── Detener cronómetro ─── */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    /* ─── Señalar fin al monitor y esperarlo ─── */
    atomic_store(&ms.done, 1);
    pthread_join(mon_tid, NULL);

    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* ─── Fase de merge: concatenar resultados de todos los hilos ─── */
    /*
     * Los hilos comprimieron bloques de filas consecutivas. Para mantener
     * la coherencia del archivo .rle, los buffers se concatenan EN ORDEN
     * (hilo 0, hilo 1, ..., hilo N-1). Esto produce un archivo idéntico
     * al que generaría la versión secuencial.
     */
    size_t total_compressed = 0;
    for (int i = 0; i < num_threads; i++)
        total_compressed += args[i].result.size;

    FILE *fout = fopen(outpath, "wb");
    if (!fout) { perror("fopen"); return 1; }
    uint32_t header[2] = { img.width, img.height };
    fwrite(header, sizeof(uint32_t), 2, fout);
    for (int i = 0; i < num_threads; i++)
        fwrite(args[i].result.data, 1, args[i].result.size, fout);
    fclose(fout);

    /* ─── Calcular métricas finales ─── */
    size_t file_size = sizeof(uint32_t) * 2 + total_compressed;
    double ratio = (1.0 - (double)file_size / raw_size) * 100.0;
    double user_t, sys_t;
    get_cpu_times(&user_t, &sys_t);
    double cpu_total = user_t + sys_t;

    /*
     * SPEEDUP: razón entre CPU total y wall time.
     * - Speedup = 1.0 → sin paralelismo (secuencial)
     * - Speedup = N   → paralelismo perfecto con N cores
     * - Speedup < N   → overhead de creación de hilos, sincronización, etc.
     *
     * También se puede calcular como T_secuencial / T_paralelo,
     * pero usar CPU/wall es más directo y no requiere ejecutar ambas versiones.
     */
    double speedup = elapsed > 0 ? cpu_total / elapsed : 0;

    /* ─── Resumen final detallado ─── */
    printf("\n");
    printf("\033[1;36m┌─── RESUMEN FINAL ──────────────────────────────────────────────────────────┐\033[0m\n");
    printf("\033[1;36m│\033[0m  Imagen:             \033[1m%u x %u\033[0m (%zu bytes)                             \033[1;36m│\033[0m\n",
           img.width, img.height, raw_size);
    printf("\033[1;36m│\033[0m  Tamaño comprimido:  \033[1;32m%zu\033[0m bytes                                           \033[1;36m│\033[0m\n", file_size);
    printf("\033[1;36m│\033[0m  Ratio compresión:   \033[1;32m%.1f%%\033[0m                                                 \033[1;36m│\033[0m\n", ratio);
    printf("\033[1;36m│\033[0m  Archivo de salida:  \033[1m%-30s\033[0m                          \033[1;36m│\033[0m\n", outpath);
    printf("\033[1;36m│\033[0m                                                                              \033[1;36m│\033[0m\n");
    printf("\033[1;36m│\033[0m  Tiempo total:       \033[1;33m%.6f\033[0m s                                          \033[1;36m│\033[0m\n", elapsed);
    printf("\033[1;36m│\033[0m  CPU total (usr):    %.6f s                                          \033[1;36m│\033[0m\n", user_t);
    printf("\033[1;36m│\033[0m  CPU total (sys):    %.6f s                                          \033[1;36m│\033[0m\n", sys_t);
    printf("\033[1;36m│\033[0m  Speedup CPU:        \033[1;33m%.2fx\033[0m (CPU time / wall time)                         \033[1;36m│\033[0m\n", speedup);
    printf("\033[1;36m│\033[0m  Hilos utilizados:   \033[1m%d\033[0m                                                   \033[1;36m│\033[0m\n", num_threads);
    printf("\033[1;36m│\033[0m                                                                              \033[1;36m│\033[0m\n");

    /* ─── Tabla de detalle por hilo ─── */
    printf("\033[1;36m│\033[0m  \033[1mDetalle por hilo:\033[0m                                                         \033[1;36m│\033[0m\n");
    printf("\033[1;36m│\033[0m  Hilo  TID         Core  Filas      Píxeles      CPU(ms)   Comprimido     \033[1;36m│\033[0m\n");
    printf("\033[1;36m│\033[0m  ───── ─────────── ────  ─────      ──────────   ────────  ────────────   \033[1;36m│\033[0m\n");

    for (int i = 0; i < num_threads; i++) {
        ThreadArg *ta = &args[i];
        printf("\033[1;36m│\033[0m   %2d   0x%-8lx   %2d   %5u      %-10zu   %6.2f    %-12zu   \033[1;36m│\033[0m\n",
               i, (unsigned long)ta->system_tid, i,
               ta->num_rows, ta->num_pixels,
               ta->cpu_time * 1000.0, ta->result.size);
    }

    printf("\033[1;36m└────────────────────────────────────────────────────────────────────────────┘\033[0m\n");

    /* ─── Liberar toda la memoria ─── */
    free(img.data);
    for (int i = 0; i < num_threads; i++)
        free(args[i].result.data);
    free(threads);
    free(args);
    return 0;
}
