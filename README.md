# Compresión RLE de Imágenes: Secuencial vs Paralelo

Proyecto de Sistemas Operativos que implementa compresión Run-Length Encoding (RLE) sobre imágenes RGB en dos versiones — secuencial (1 hilo) y paralela (pthreads, 1 hilo por core) — para analizar el impacto del paralelismo en rendimiento.

---

## Tabla de Contenidos

1. [Requisitos del Sistema](#requisitos-del-sistema)
2. [Instalación y Compilación](#instalación-y-compilación)
3. [Manual de Uso](#manual-de-uso)
4. [Arquitectura del Código](#arquitectura-del-código)
5. [Algoritmo RLE](#algoritmo-rle)
6. [Estrategia de Paralelización](#estrategia-de-paralelización)
7. [Métricas Reportadas](#métricas-reportadas)
8. [Análisis Comparativo](#análisis-comparativo)
9. [Formato de Archivos](#formato-de-archivos)
10. [Conclusiones sobre Paralelización](#conclusiones-sobre-paralelización)

---

## Requisitos del Sistema

### Software
- **Compilador C**: GCC o Clang con soporte para C11 (`stdatomic.h`)
- **Make**: GNU Make o compatible
- **Bash**: Para el script de benchmark (versión 3.2+)
- **bc**: Calculadora de línea de comandos (para el benchmark)

### Hardware (recomendado)
- CPU multi-core (2+ cores para apreciar el paralelismo)
- 512 MB de RAM libre (la imagen sintética consume ~50 MB)

### Plataformas soportadas
| Plataforma | Estado | Notas |
|------------|--------|-------|
| macOS (Apple Silicon) | Completo | Mach APIs para métricas por hilo |
| macOS (Intel) | Completo | Mach APIs para métricas por hilo |
| Linux (x86_64) | Completo | /proc para métricas, CLOCK_THREAD_CPUTIME_ID |

---

## Instalación y Compilación

### Paso 1: Obtener el código fuente

```bash
cd /ruta/al/proyecto
ls *.c Makefile
# Debe contener: rle_secuencial.c  rle_paralelo.c  Makefile
```

### Paso 2: Compilar

```bash
# Compilar ambos programas
make all

# O compilar individualmente
make secuencial   # Solo rle_secuencial
make paralelo     # Solo rle_paralelo
```

### Paso 3: Verificar

```bash
ls -la rle_secuencial rle_paralelo
# Ambos ejecutables deben existir
```

### Limpieza

```bash
make clean   # Elimina ejecutables y archivos .rle
```

---

## Manual de Uso

### Ejecución básica (imagen sintética)

```bash
# Versión secuencial — genera imagen sintética 4096×4096 automáticamente
./rle_secuencial

# Versión paralela — misma imagen, múltiples hilos
./rle_paralelo
```

### Ejecución con archivo PPM

```bash
# Comprimir un archivo PPM con la versión secuencial
./rle_secuencial foto.ppm

# Comprimir el mismo archivo con la versión paralela
./rle_paralelo foto.ppm
```

### Benchmark automático

```bash
# Dar permisos de ejecución
chmod +x benchmark.sh

# Ejecutar con 5 iteraciones (por defecto)
./benchmark.sh

# Ejecutar con 10 iteraciones
./benchmark.sh 10

# Ejecutar con archivo PPM
./benchmark.sh 5 foto.ppm
```

El benchmark genera el archivo `reporte_analisis.txt` con el análisis completo.

### Verificar corrección

```bash
# Ejecutar ambas versiones y comparar salida
./rle_secuencial
./rle_paralelo
diff output_secuencial.rle output_paralelo.rle
# No debe producir salida (archivos idénticos)
```

---

## Arquitectura del Código

### Estructura de archivos

```
.
├── rle_secuencial.c       # Versión secuencial (1 hilo)
├── rle_paralelo.c         # Versión paralela (N hilos con pthreads)
├── Makefile               # Sistema de compilación
├── benchmark.sh           # Script de análisis comparativo
├── README.md              # Esta documentación
└── reporte_analisis.txt   # Generado por benchmark.sh
```

### Organización del código (ambos archivos)

Cada archivo `.c` está organizado en 9 secciones claramente delimitadas:

| Sección | Contenido |
|---------|-----------|
| 1. Estructuras de datos | `Image`, `Buffer`, `Progress`/`ThreadArg` |
| 2. Buffer dinámico | `buffer_init`, `buffer_push` |
| 3. Métricas del sistema | `get_rss`, `get_cpu_times`, `get_thread_cpu` |
| 4. Lectura PPM | `load_ppm` — parser de formato P6 |
| 5. Imagen sintética | `generate_synthetic` — patrones de prueba |
| 6. Visualización | `print_bar`, `print_display` — UI en terminal |
| 7. Hilo monitor | `monitor_func` — refresco periódico del display |
| 8. Compresión RLE | `rle_compress` / `rle_thread_func` |
| 9. Función principal | `main` — orquestación del flujo |

---

## Algoritmo RLE

### Descripción

Run-Length Encoding identifica secuencias consecutivas de valores idénticos y las reemplaza por un par (contador, valor):

```
Entrada:  AAAAAABBBCCCCCCCC
RLE:      (6,A)(3,B)(8,C)
```

### Aplicación a píxeles RGB

Cada píxel tiene 3 componentes (R, G, B). Un "run" es una secuencia de píxeles consecutivos con exactamente el mismo color.

**Formato de un run:**
```
[count: 1 byte][R: 1 byte][G: 1 byte][B: 1 byte] = 4 bytes
```

- `count`: 1 a 255 (0 no se usa)
- Si hay más de 255 píxeles iguales consecutivos, se emiten múltiples runs

### Complejidad

| Aspecto | Valor |
|---------|-------|
| Temporal | O(n) — cada píxel se examina una vez |
| Espacial | O(n) en el peor caso (buffer de salida) |
| Mejor caso | Imagen monocolor → compresión 99.99% |
| Peor caso | Cada píxel diferente → expansión del 33% |

### Ejemplo numérico

Imagen 4096×4096 con bandas horizontales uniformes:
- Original: 4096 × 4096 × 3 = 50,331,648 bytes (48 MB)
- Cada fila: 4096 píxeles iguales → ceil(4096/255) = 17 runs × 4 bytes = 68 bytes
- Total: 4096 filas × 68 bytes ≈ 278,528 bytes (272 KB)
- Compresión: **99.5%**

---

## Estrategia de Paralelización

### División del trabajo

La imagen se divide en bloques horizontales de filas consecutivas:

```
┌─────────────────────────────────┐
│     Bloque 0 (Hilo 0)          │ filas 0-511      → 2,097,152 píxeles
├─────────────────────────────────┤
│     Bloque 1 (Hilo 1)          │ filas 512-1023   → 2,097,152 píxeles
├─────────────────────────────────┤
│     Bloque 2 (Hilo 2)          │ filas 1024-1535  → 2,097,152 píxeles
├─────────────────────────────────┤
│            ...                  │
├─────────────────────────────────┤
│     Bloque 7 (Hilo 7)          │ filas 3584-4095  → 2,097,152 píxeles
└─────────────────────────────────┘
```

### ¿Por qué funciona sin sincronización?

1. **Lectura compartida sin conflicto**: Todos los hilos leen de la imagen original, pero cada uno lee una región disjunta. No hay escrituras al arreglo de entrada.

2. **Escritura independiente**: Cada hilo escribe en su propio buffer local (`Buffer result` en `ThreadArg`). No hay escrituras compartidas.

3. **Progreso lock-free**: La variable `atomic_size_t pixels_done` se actualiza con `atomic_store` (operación lock-free de ~1ns). El monitor lee con `atomic_load`.

4. **Merge ordenado**: Después del join, los buffers se concatenan en orden de hilo (0, 1, ..., N-1), lo que produce el mismo resultado que la versión secuencial.

### Flujo de ejecución paralelo

```
Hilo principal    Hilo 0     Hilo 1     ...    Hilo N-1    Monitor
     │               │          │                 │           │
     ├─create────────>│          │                 │           │
     ├─create─────────┼─────────>│                 │           │
     ├─ ...           │          │                 │           │
     ├─create─────────┼──────────┼─────────────────>           │
     ├─create─────────┼──────────┼─────────────────┼──────────>│
     │                │          │                 │           │
     │              RLE(B0)    RLE(B1)   ...    RLE(BN)     display
     │              RLE(B0)    RLE(B1)   ...    RLE(BN)     display
     │              RLE(B0)    RLE(B1)   ...    RLE(BN)     display
     │                │          │                 │           │
     ├<──join─────────┘          │                 │           │
     ├<──join────────────────────┘                 │           │
     ├─ ...                                        │           │
     ├<──join──────────────────────────────────────┘           │
     │                                                         │
     ├──done=1─────────────────────────────────────────────────>│
     ├<──join──────────────────────────────────────────────────┘
     │
     ├── merge (concatenar buffers en orden)
     ├── escribir archivo .rle
     └── imprimir resumen final
```

### Afinidad de cores (macOS)

```c
thread_affinity_policy_data_t policy = { i + 1 };  // Tag único por hilo
thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, &policy, ...);
```

Hilos con tags diferentes reciben la "sugerencia" del kernel de ejecutarse en cores separados. El tag 0 significa "sin preferencia", por eso usamos i+1.

---

## Métricas Reportadas

### Versión secuencial

| Métrica | Fuente | Descripción |
|---------|--------|-------------|
| Progreso | `atomic_size_t` | Barra visual actualizada cada 80ms |
| Tiempo total | `CLOCK_MONOTONIC` | Wall clock time (no afectado por cambios NTP) |
| CPU usuario | `getrusage` | Tiempo en código de aplicación |
| CPU sistema | `getrusage` | Tiempo en llamadas al kernel |
| Uso CPU % | Calculado | `(usr+sys) / wall × 100`. Esperado: ≤100% |
| Memoria RSS | Mach `task_info` | RAM física consumida |
| Throughput | Calculado | MB procesados por segundo |

### Versión paralela (adicional)

| Métrica | Fuente | Descripción |
|---------|--------|-------------|
| TID por hilo | `pthread_threadid_np` | ID nativo del sistema operativo |
| CPU por hilo | Mach `thread_info` | ms de CPU consumidos individualmente |
| Progreso por hilo | `atomic_size_t` | Barra visual independiente por hilo |
| Speedup | Calculado | `cpu_total / wall_time` (>1 = paralelismo real) |
| Uso CPU % | Calculado | Puede superar 100% (N cores × 100%) |

---

## Análisis Comparativo

### Cómo demostrar el paralelismo

La **evidencia clave** de que se están usando múltiples cores es:

1. **CPU total > Wall time**: Si la suma de CPU de todos los hilos supera el tiempo real transcurrido, necesariamente están ejecutándose en paralelo.

2. **Uso de CPU > 100%**: Un valor de 400% significa que se están usando 4 cores simultáneamente.

3. **Progreso simultáneo**: En el display en tiempo real, todas las barras de progreso avanzan al mismo tiempo.

4. **Tiempos de CPU similares por hilo**: Si todos los hilos consumen ~2-3ms de CPU, están ejecutándose en paralelo, no secuencialmente.

### Métricas típicas observadas

| Métrica | Secuencial | Paralelo (8 cores) |
|---------|-----------|---------------------|
| Wall time | ~23 ms | ~5.6 ms |
| CPU total | ~24 ms | ~30 ms |
| Uso CPU | ~100% | ~530% |
| Speedup | 1.0x | ~4.1x |
| Memoria | ~50 MB | ~50 MB |
| Resultado | 264,200 bytes | 264,200 bytes (idéntico) |

---

## Formato de Archivos

### Entrada: PPM P6

```
P6\n                    ← Magic number (ASCII)
# comentario\n          ← Opcional, cualquier cantidad
ancho alto\n            ← Dimensiones en ASCII
maxval\n                ← Típicamente 255
<datos binarios>        ← width × height × 3 bytes (RGB)
```

### Salida: .rle

```
Offset 0:   uint32_t width      (4 bytes, little-endian en x86)
Offset 4:   uint32_t height     (4 bytes)
Offset 8+:  runs RLE            (secuencia de bloques de 4 bytes)
              [count][R][G][B]   count: 1-255
              [count][R][G][B]
              ...
```

---

## Conclusiones sobre Paralelización

### Cuándo ES apropiado paralelizar

| Condición | Aplica a este proyecto |
|-----------|----------------------|
| Problema "embarrassingly parallel" | Sí — cada bloque de filas es independiente |
| Alto volumen de datos | Sí — 48 MB de datos procesados |
| CPU-bound (no I/O-bound) | Sí — la compresión es puro cómputo |
| Baja proporción de código serial | Sí — solo el merge es serial (~0.1% del tiempo) |
| Memoria independiente por hilo | Sí — cada hilo tiene su propio buffer |

### Cuándo NO es apropiado paralelizar

- **Datasets pequeños**: Si la imagen es <100KB, el overhead de crear hilos (~10μs) supera el beneficio.
- **Tareas I/O-bound**: Si el cuello de botella es disco o red, más hilos de CPU no ayudan.
- **Dependencias de datos**: Si el resultado del píxel N depende del píxel N-1 (ej. compresión LZ77), no se puede dividir fácilmente.
- **Sincronización compleja**: Si se necesitan mutex, barreras o variables de condición frecuentes, el overhead puede eliminar la ganancia.

### Ley de Amdahl

El speedup máximo teórico está limitado por la fracción serial del programa:

```
Speedup_max = 1 / (S + (1-S)/N)
```

Donde S = fracción serial, N = número de cores.

Para este proyecto, S ≈ 0.01 (solo merge y I/O de archivo):
- Con 4 cores: speedup max ≈ 3.88x
- Con 8 cores: speedup max ≈ 7.47x
- Con 16 cores: speedup max ≈ 13.91x

### Consideraciones de overhead

| Fuente de overhead | Impacto | Mitigación aplicada |
|-------------------|---------|---------------------|
| Creación de hilos | ~10 μs por hilo | Se crean solo N hilos |
| Operaciones atómicas | ~1 ns por operación | Solo al final de cada run |
| False sharing | Variable | ThreadArgs separados en heap |
| Cache misses | Bajo | Acceso secuencial por bloque |
| Merge de resultados | O(N) | Escritura secuencial a disco |
