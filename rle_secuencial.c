/*
 * ============================================================================
 *  rle_secuencial.c — Compresión RLE de Imágenes (Versión Secuencial)
 * ============================================================================
 *
 *  DESCRIPCIÓN:
 *      Implementa el algoritmo de compresión Run-Length Encoding (RLE) sobre
 *      imágenes RGB utilizando un único hilo de ejecución. Sirve como línea
 *      base para comparar con la versión paralela (rle_paralelo.c).
 *
 *  ALGORITMO RLE:
 *      RLE identifica secuencias consecutivas de píxeles idénticos (runs) y
 *      las codifica como tuplas (contador, R, G, B). Por ejemplo, 200 píxeles
 *      rojos consecutivos se almacenan como (200, 255, 0, 0) = 4 bytes en
 *      lugar de 200 × 3 = 600 bytes.
 *
 *      Formato de cada run: [count: 1 byte][R: 1 byte][G: 1 byte][B: 1 byte]
 *      - count: número de repeticiones (1-255)
 *      - R, G, B: valores del color del píxel
 *
 *  FORMATO DE ARCHIVO .rle:
 *      Offset 0:  uint32_t width    (ancho en píxeles)
 *      Offset 4:  uint32_t height   (alto en píxeles)
 *      Offset 8:  datos RLE comprimidos (secuencia de runs de 4 bytes)
 *
 *  ENTRADA SOPORTADA:
 *      - Archivos PPM formato P6 (binario, 8 bits por canal)
 *      - Sin archivo: genera imagen sintética 4096×4096 con patrones repetitivos
 *
 *  MÉTRICAS REPORTADAS:
 *      - Progreso en tiempo real (barra visual)
 *      - Tiempo de ejecución (wall clock vía CLOCK_MONOTONIC)
 *      - Tiempo de CPU usuario y sistema (vía getrusage)
 *      - Porcentaje de uso de CPU
 *      - Memoria RSS del proceso (vía Mach APIs en macOS, /proc en Linux)
 *      - Throughput en MB/s
 *      - Tamaño comprimido y ratio de compresión
 *
 *  COMPILACIÓN:
 *      gcc -Wall -Wextra -O2 -o rle_secuencial rle_secuencial.c -lpthread
 *
 *  USO:
 *      ./rle_secuencial                  # imagen sintética 4096×4096
 *      ./rle_secuencial imagen.ppm       # archivo PPM real
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
#include <stdatomic.h>      /* atomic_size_t — comunicación segura entre hilos */
#include <pthread.h>        /* pthread_create/join — hilo monitor */

#ifdef __APPLE__
#include <mach/mach.h>      /* task_info — memoria RSS en macOS */
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 1: ESTRUCTURAS DE DATOS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Image: Representa una imagen RGB en memoria.
 *
 * La imagen se almacena como un arreglo lineal de bytes donde cada píxel
 * ocupa 3 bytes consecutivos (R, G, B). El píxel en la posición (x, y) se
 * encuentra en el índice: (y * width + x) * 3
 *
 * Ejemplo para imagen 2×2:
 *   data[0..2]  = píxel (0,0) → R, G, B
 *   data[3..5]  = píxel (1,0) → R, G, B
 *   data[6..8]  = píxel (0,1) → R, G, B
 *   data[9..11] = píxel (1,1) → R, G, B
 */
typedef struct {
    uint32_t width;   /* Ancho en píxeles */
    uint32_t height;  /* Alto en píxeles */
    uint8_t *data;    /* Datos RGB: width × height × 3 bytes */
} Image;

/*
 * Buffer: Buffer dinámico que crece automáticamente.
 *
 * Se usa para acumular los datos comprimidos RLE sin conocer de antemano
 * el tamaño final. Comienza con una capacidad estimada y se duplica
 * cada vez que se necesita más espacio (estrategia de crecimiento amortizado).
 */
typedef struct {
    uint8_t *data;     /* Puntero a los datos almacenados */
    size_t size;       /* Bytes actualmente escritos */
    size_t capacity;   /* Capacidad total asignada */
} Buffer;

/*
 * Progress: Estado compartido entre el hilo de compresión y el hilo monitor.
 *
 * Usa variables atómicas (C11 stdatomic.h) para comunicación segura entre
 * hilos sin necesidad de mutex, ya que las operaciones son simples
 * lecturas/escrituras de valores escalares.
 *
 * El hilo de compresión actualiza pixels_processed y compressed_bytes.
 * El hilo monitor lee estos valores periódicamente para mostrar el progreso.
 */
typedef struct {
    atomic_size_t pixels_processed;  /* Píxeles procesados hasta ahora */
    atomic_size_t compressed_bytes;  /* Bytes de salida generados */
    size_t total_pixels;             /* Total de píxeles a procesar (constante) */
    atomic_int done;                 /* Flag: 1 cuando la compresión terminó */
} Progress;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 2: BUFFER DINÁMICO
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * buffer_init: Inicializa un buffer con capacidad inicial.
 *
 * @param buf  Buffer a inicializar
 * @param cap  Capacidad inicial en bytes
 *
 * La capacidad inicial debe estimarse para minimizar realocaciones.
 * Para RLE, una buena estimación es raw_size/2 (asumiendo compresión 2:1).
 */
static void buffer_init(Buffer *buf, size_t cap) {
    buf->data = malloc(cap);
    if (!buf->data) {
        perror("malloc");
        exit(1);
    }
    buf->size = 0;
    buf->capacity = cap;
}

/*
 * buffer_push: Agrega bytes al final del buffer, expandiéndolo si es necesario.
 *
 * @param buf    Buffer destino
 * @param bytes  Datos a agregar
 * @param n      Cantidad de bytes a agregar
 *
 * Si la capacidad actual no alcanza, se duplica repetidamente hasta que sea
 * suficiente. La complejidad amortizada de N inserciones es O(N).
 */
static void buffer_push(Buffer *buf, const uint8_t *bytes, size_t n) {
    while (buf->size + n > buf->capacity) {
        buf->capacity *= 2;
        buf->data = realloc(buf->data, buf->capacity);
        if (!buf->data) {
            perror("realloc");
            exit(1);
        }
    }
    memcpy(buf->data + buf->size, bytes, n);
    buf->size += n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 3: MÉTRICAS DEL SISTEMA
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * get_rss: Obtiene la memoria física (RSS - Resident Set Size) del proceso.
 *
 * @return  Bytes de memoria física utilizados
 *
 * En macOS: usa Mach API (task_info con MACH_TASK_BASIC_INFO)
 * En Linux: lee /proc/self/statm (segunda columna = páginas residentes)
 *
 * RSS indica cuánta memoria RAM física está consumiendo el proceso,
 * excluyendo páginas en swap. Es la métrica más relevante para evaluar
 * el impacto en memoria de la aplicación.
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
 * get_cpu_times: Obtiene los tiempos de CPU acumulados del proceso.
 *
 * @param user_s  [salida] Segundos en modo usuario
 * @param sys_s   [salida] Segundos en modo sistema (kernel)
 *
 * Tiempo usuario: CPU gastado ejecutando código de la aplicación.
 * Tiempo sistema: CPU gastado en llamadas al kernel (I/O, memoria, etc.).
 *
 * El porcentaje de uso de CPU se calcula como:
 *   cpu_pct = (user + system) / wall_time × 100
 *
 * Para un programa secuencial, este valor no debería superar ~100%.
 * Valores menores indican tiempo esperando I/O o sincronización.
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

/*
 * load_ppm: Carga una imagen desde un archivo PPM formato P6.
 *
 * @param path  Ruta al archivo PPM
 * @param img   [salida] Estructura Image donde se almacenará la imagen
 * @return      0 en éxito, -1 en error
 *
 * El formato PPM P6 es un formato de imagen sin compresión:
 *   - Línea 1: "P6" (magic number)
 *   - Líneas opcionales: comentarios precedidos por '#'
 *   - Ancho y alto en ASCII
 *   - Valor máximo por canal (típicamente 255)
 *   - Un byte de espacio en blanco
 *   - Datos binarios RGB: width × height × 3 bytes
 *
 * Ejemplo de header PPM:
 *   P6
 *   # Esto es un comentario
 *   640 480
 *   255
 *   [datos binarios...]
 */
static int load_ppm(const char *path, Image *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Verificar magic number "P6" */
    char magic[3];
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fclose(f);
        return -1;
    }

    /* Saltar comentarios (líneas que comienzan con '#') y espacios en blanco */
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            /* Consumir hasta fin de línea */
            while ((c = fgetc(f)) != EOF && c != '\n');
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        } else {
            ungetc(c, f);
            break;
        }
    }

    /* Leer dimensiones y valor máximo */
    int w, h, maxval;
    if (fscanf(f, "%d %d %d", &w, &h, &maxval) != 3) {
        fclose(f);
        return -1;
    }
    fgetc(f); /* Consumir el único carácter de espacio después de maxval */

    /* Asignar memoria y leer datos de píxeles */
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    size_t pixel_bytes = (size_t)w * h * 3;
    img->data = malloc(pixel_bytes);
    if (!img->data) {
        fclose(f);
        return -1;
    }

    if (fread(img->data, 1, pixel_bytes, f) != pixel_bytes) {
        free(img->data);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 5: GENERACIÓN DE IMAGEN SINTÉTICA
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * generate_synthetic: Genera una imagen de prueba con patrones repetitivos.
 *
 * @param img  [salida] Estructura Image donde se genera la imagen
 * @param w    Ancho deseado en píxeles
 * @param h    Alto deseado en píxeles
 *
 * La imagen se genera con bandas horizontales de 8 píxeles de alto,
 * donde cada banda tiene un color uniforme. Esto crea runs largos que
 * son ideales para RLE (cada fila completa es un único run de 4096 píxeles
 * iguales, y 8 filas consecutivas comparten el mismo color).
 *
 * Los colores se generan con una fórmula determinista basada en el
 * índice de banda para garantizar reproducibilidad:
 *   R = (banda × 37) mod 256
 *   G = (banda × 59) mod 256
 *   B = (banda × 91) mod 256
 *
 * Para una imagen 4096×4096:
 *   - Tamaño sin comprimir: 4096 × 4096 × 3 = 48 MB
 *   - Bandas: 4096/8 = 512 colores diferentes
 *   - Tamaño comprimido esperado: ~512 colores × (4096/255 + 1) runs ≈ 264 KB
 */
static void generate_synthetic(Image *img, uint32_t w, uint32_t h) {
    img->width = w;
    img->height = h;
    size_t pixel_bytes = (size_t)w * h * 3;
    img->data = malloc(pixel_bytes);
    if (!img->data) {
        perror("malloc");
        exit(1);
    }

    for (uint32_t y = 0; y < h; y++) {
        /* Calcular color para esta banda (cambia cada 8 filas) */
        uint32_t band = y / 8;
        uint8_t r = (uint8_t)(band * 37 % 256);
        uint8_t g = (uint8_t)(band * 59 % 256);
        uint8_t b = (uint8_t)(band * 91 % 256);

        /* Rellenar toda la fila con el mismo color */
        for (uint32_t x = 0; x < w; x++) {
            size_t idx = ((size_t)y * w + x) * 3;
            img->data[idx]     = r;
            img->data[idx + 1] = g;
            img->data[idx + 2] = b;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 6: VISUALIZACIÓN EN TIEMPO REAL
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Ancho de la barra de progreso en caracteres */
#define BAR_WIDTH 40

/* Número total de líneas que ocupa el display (para movimiento de cursor) */
#define DISPLAY_LINES 14

/*
 * print_bar: Dibuja una barra de progreso con caracteres Unicode.
 *
 * @param pct  Porcentaje de progreso (0.0 a 100.0)
 *
 * Usa bloques Unicode (█ y ░) con colores ANSI:
 *   - Verde (32) para la parte completada
 *   - Gris oscuro (90) para la parte pendiente
 *
 * Ejemplo con 60%: ████████████████████████░░░░░░░░░░░░░░░░
 */
static void print_bar(double pct) {
    int filled = (int)(pct / 100.0 * BAR_WIDTH);
    if (filled > BAR_WIDTH) filled = BAR_WIDTH;

    printf("\033[32m");  /* Color verde */
    for (int i = 0; i < filled; i++) printf("█");
    printf("\033[90m");  /* Color gris oscuro */
    for (int i = filled; i < BAR_WIDTH; i++) printf("░");
    printf("\033[0m");   /* Reset colores */
}

/*
 * print_display: Dibuja el panel completo de métricas en tiempo real.
 *
 * @param prog     Estado de progreso (compartido con hilo de compresión)
 * @param t_start  Tiempo de inicio para calcular elapsed time
 * @param raw_size Tamaño original en bytes (para calcular throughput)
 *
 * El display se encierra en un recuadro Unicode y muestra:
 *   1. Tamaño original de los datos
 *   2. Barra de progreso visual con porcentaje
 *   3. Tiempo transcurrido (wall clock)
 *   4. Tiempo CPU desglosado (usuario vs sistema)
 *   5. Porcentaje de uso de CPU
 *   6. Memoria RSS actual
 *   7. Throughput de procesamiento
 *   8. Bytes comprimidos generados
 *
 * Usa secuencias de escape ANSI para colores y posicionamiento:
 *   \033[36m  → Cian (bordes del recuadro)
 *   \033[1m   → Negrita (valores numéricos)
 *   \033[33m  → Amarillo (títulos)
 *   \033[0m   → Reset
 */
static void print_display(Progress *prog, struct timespec *t_start, size_t raw_size) {
    /* Calcular tiempo transcurrido con precisión de nanosegundos */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - t_start->tv_sec) +
                     (now.tv_nsec - t_start->tv_nsec) / 1e9;

    /* Leer estado atómico del progreso */
    size_t processed = atomic_load(&prog->pixels_processed);
    size_t comp_bytes = atomic_load(&prog->compressed_bytes);
    double pct = prog->total_pixels > 0
                 ? (double)processed / prog->total_pixels * 100.0 : 0;
    if (pct > 100.0) pct = 100.0;

    /* Obtener métricas de CPU y memoria */
    double user_t, sys_t;
    get_cpu_times(&user_t, &sys_t);
    double cpu_pct = elapsed > 0.001 ? (user_t + sys_t) / elapsed * 100.0 : 0;
    size_t rss = get_rss();
    double throughput = elapsed > 0.001
                        ? (processed * 3.0 / (1024.0 * 1024.0)) / elapsed : 0;

    /* Dibujar recuadro con métricas */
    printf("\033[36m╔══════════════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[36m║\033[0m   \033[1;33mCOMPRESIÓN RLE — MODO SECUENCIAL (1 hilo)\033[0m                \033[36m║\033[0m\n");
    printf("\033[36m╠══════════════════════════════════════════════════════════════╣\033[0m\n");
    printf("\033[36m║\033[0m  Datos originales: \033[1m%-10zu\033[0m bytes (%zu píxeles)      \033[36m║\033[0m\n",
           raw_size, prog->total_pixels);
    printf("\033[36m║\033[0m                                                            \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m  Progreso: ");
    print_bar(pct);
    printf(" %5.1f%%", pct);
    printf("         \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m                                                            \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m  Tiempo transcurrido:  \033[1m%8.6f\033[0m s                        \033[36m║\033[0m\n", elapsed);
    printf("\033[36m║\033[0m  Tiempo CPU (usuario): \033[1m%8.6f\033[0m s                        \033[36m║\033[0m\n", user_t);
    printf("\033[36m║\033[0m  Tiempo CPU (sistema): \033[1m%8.6f\033[0m s                        \033[36m║\033[0m\n", sys_t);
    printf("\033[36m║\033[0m  Uso de CPU:           \033[1m%6.1f%%\033[0m                            \033[36m║\033[0m\n", cpu_pct);
    printf("\033[36m║\033[0m  Memoria RSS:          \033[1m%6.1f\033[0m MB                          \033[36m║\033[0m\n",
           rss / (1024.0 * 1024.0));
    printf("\033[36m║\033[0m  Throughput:           \033[1m%7.1f\033[0m MB/s                       \033[36m║\033[0m\n", throughput);
    printf("\033[36m║\033[0m  Datos comprimidos:    \033[1m%-10zu\033[0m bytes                    \033[36m║\033[0m\n", comp_bytes);
    printf("\033[36m╚══════════════════════════════════════════════════════════════╝\033[0m\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 7: HILO MONITOR
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * MonitorArg: Argumentos para el hilo monitor.
 *
 * El hilo monitor es un hilo auxiliar cuya única función es refrescar
 * la visualización periódicamente. No participa en la compresión.
 *
 * NOTA: Aunque esta es la versión "secuencial", el hilo monitor NO afecta
 * la medición de rendimiento porque:
 *   1. El tiempo se mide solo para la compresión (entre clock_gettime calls)
 *   2. El monitor solo lee datos atómicos y hace I/O a terminal
 *   3. El CPU reportado es del proceso completo, no solo del hilo principal
 */
typedef struct {
    Progress *prog;
    struct timespec *t_start;
    size_t raw_size;
} MonitorArg;

/*
 * monitor_func: Función ejecutada por el hilo monitor.
 *
 * Ciclo principal:
 *   1. Mover cursor arriba N líneas (sobreescribir display anterior)
 *   2. Dibujar el display actualizado
 *   3. Esperar 80ms
 *   4. Repetir hasta que prog->done sea 1
 *
 * El movimiento de cursor usa la secuencia ANSI: \033[NA (mover N líneas arriba)
 */
static void *monitor_func(void *arg) {
    MonitorArg *ma = (MonitorArg *)arg;
    int first = 1;

    while (!atomic_load(&ma->prog->done)) {
        /* En iteraciones posteriores, mover cursor arriba para sobreescribir */
        if (!first) printf("\033[%dA", DISPLAY_LINES + 1);
        first = 0;

        print_display(ma->prog, ma->t_start, ma->raw_size);
        fflush(stdout);
        usleep(80000); /* 80ms entre refrescos (~12.5 FPS) */
    }

    /* Refresco final con datos completos */
    if (!first) printf("\033[%dA", DISPLAY_LINES + 1);
    print_display(ma->prog, ma->t_start, ma->raw_size);
    fflush(stdout);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECCIÓN 8: ALGORITMO DE COMPRESIÓN RLE
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * rle_compress: Comprime un arreglo de píxeles RGB usando Run-Length Encoding.
 *
 * @param pixels      Datos de entrada (RGB, 3 bytes por píxel)
 * @param num_pixels  Número total de píxeles a procesar
 * @param out         Buffer de salida donde se escriben los runs comprimidos
 * @param prog        Estado de progreso (actualizado atómicamente)
 *
 * Algoritmo:
 *   1. Tomar el color del píxel actual
 *   2. Contar cuántos píxeles consecutivos tienen el mismo color
 *   3. Limitar el conteo a 255 (máximo almacenable en 1 byte)
 *   4. Escribir el run: (count, R, G, B) = 4 bytes
 *   5. Avanzar al siguiente píxel diferente
 *   6. Repetir hasta procesar todos los píxeles
 *
 * Complejidad: O(n) donde n = número de píxeles
 * Cada píxel se examina exactamente una vez.
 *
 * Caso mejor: imagen de un solo color → 1 run por cada 255 píxeles
 *   Compresión: 3n bytes → 4 × ceil(n/255) bytes
 *   Para n=16M: 48MB → 262KB (99.5% de compresión)
 *
 * Caso peor: cada píxel diferente al anterior → 1 run por píxel
 *   Compresión: 3n bytes → 4n bytes (expansión del 33%)
 */
static void rle_compress(const uint8_t *pixels, size_t num_pixels,
                         Buffer *out, Progress *prog) {
    size_t i = 0;

    while (i < num_pixels) {
        /* Obtener color del píxel actual */
        uint8_t r = pixels[i * 3];
        uint8_t g = pixels[i * 3 + 1];
        uint8_t b = pixels[i * 3 + 2];

        /* Contar píxeles consecutivos con el mismo color */
        uint8_t count = 1;
        while (i + count < num_pixels && count < 255 &&
               pixels[(i + count) * 3]     == r &&
               pixels[(i + count) * 3 + 1] == g &&
               pixels[(i + count) * 3 + 2] == b) {
            count++;
        }

        /* Escribir run: [count, R, G, B] */
        uint8_t run[4] = { count, r, g, b };
        buffer_push(out, run, 4);

        /* Avanzar posición */
        i += count;

        /* Actualizar progreso atómico para el hilo monitor */
        atomic_store(&prog->pixels_processed, i);
        atomic_store(&prog->compressed_bytes, out->size);
    }
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

    /* ─── Determinar nombre del archivo de salida ─── */
    char outpath[512];
    if (argc >= 2)
        snprintf(outpath, sizeof(outpath), "%s.rle", argv[1]);
    else
        snprintf(outpath, sizeof(outpath), "output_secuencial.rle");

    /* ─── Inicializar estado de progreso ─── */
    Progress prog;
    atomic_init(&prog.pixels_processed, 0);
    atomic_init(&prog.compressed_bytes, 0);
    prog.total_pixels = total_pixels;
    atomic_init(&prog.done, 0);

    printf("\n"); /* Línea en blanco antes del display */

    /* ─── Iniciar cronómetro ─── */
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* ─── Lanzar hilo monitor de visualización ─── */
    pthread_t mon_tid;
    MonitorArg ma = { &prog, &t_start, raw_size };
    pthread_create(&mon_tid, NULL, monitor_func, &ma);

    /* ─── COMPRESIÓN RLE (un solo hilo) ─── */
    Buffer compressed;
    buffer_init(&compressed, raw_size / 2);
    rle_compress(img.data, total_pixels, &compressed, &prog);

    /* ─── Detener cronómetro ─── */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    /* ─── Señalar fin al monitor y esperarlo ─── */
    atomic_store(&prog.done, 1);
    pthread_join(mon_tid, NULL);

    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* ─── Escribir archivo .rle comprimido ─── */
    FILE *fout = fopen(outpath, "wb");
    if (!fout) {
        perror("fopen");
        free(img.data);
        free(compressed.data);
        return 1;
    }

    /* Header: ancho y alto como uint32_t */
    uint32_t header[2] = { img.width, img.height };
    fwrite(header, sizeof(uint32_t), 2, fout);

    /* Datos comprimidos */
    fwrite(compressed.data, 1, compressed.size, fout);
    fclose(fout);

    /* ─── Calcular métricas finales ─── */
    size_t file_size = sizeof(uint32_t) * 2 + compressed.size;
    double ratio = (1.0 - (double)file_size / raw_size) * 100.0;
    double user_t, sys_t;
    get_cpu_times(&user_t, &sys_t);

    /* ─── Imprimir resumen final ─── */
    printf("\n");
    printf("\033[1;36m┌─── RESUMEN FINAL ──────────────────────────────────────────┐\033[0m\n");
    printf("\033[1;36m│\033[0m  Imagen:             \033[1m%u x %u\033[0m (%zu bytes)            \033[1;36m│\033[0m\n",
           img.width, img.height, raw_size);
    printf("\033[1;36m│\033[0m  Tamaño comprimido:  \033[1;32m%zu\033[0m bytes                          \033[1;36m│\033[0m\n", file_size);
    printf("\033[1;36m│\033[0m  Ratio compresión:   \033[1;32m%.1f%%\033[0m                                 \033[1;36m│\033[0m\n", ratio);
    printf("\033[1;36m│\033[0m  Archivo de salida:  \033[1m%-30s\033[0m             \033[1;36m│\033[0m\n", outpath);
    printf("\033[1;36m│\033[0m  Tiempo total:       \033[1;33m%.6f\033[0m s                         \033[1;36m│\033[0m\n", elapsed);
    printf("\033[1;36m│\033[0m  CPU usuario:        %.6f s                         \033[1;36m│\033[0m\n", user_t);
    printf("\033[1;36m│\033[0m  CPU sistema:        %.6f s                         \033[1;36m│\033[0m\n", sys_t);
    printf("\033[1;36m│\033[0m  Hilos utilizados:   \033[1m1\033[0m                                      \033[1;36m│\033[0m\n");
    printf("\033[1;36m└────────────────────────────────────────────────────────────┘\033[0m\n");

    /* ─── Liberar memoria ─── */
    free(img.data);
    free(compressed.data);
    return 0;
}
