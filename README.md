# Compresión RLE de Imágenes: Secuencial vs Paralelo

Proyecto de Sistemas Operativos (UNSAAC) que implementa compresión Run-Length Encoding (RLE) sobre imágenes RGB en dos versiones — secuencial (1 hilo) y paralela (pthreads, 1 hilo por core) — para analizar el impacto del paralelismo en rendimiento y demostrar conceptos de sistemas operativos.

---

## Tabla de Contenidos

1. [Requisitos del Sistema](#requisitos-del-sistema)
2. [Instalación y Compilación](#instalación-y-compilación)
3. [Manual de Uso](#manual-de-uso)
4. [Arquitectura del Código](#arquitectura-del-código)
5. [Herramientas de Análisis](#herramientas-de-análisis)
6. [Conceptos de SO Demostrados](#conceptos-de-so-demostrados)
7. [Algoritmo RLE](#algoritmo-rle)
8. [Estrategia de Paralelización](#estrategia-de-paralelización)
9. [Métricas Reportadas](#métricas-reportadas)
10. [Análisis Comparativo](#análisis-comparativo)
11. [Formato de Archivos](#formato-de-archivos)
12. [Estructura del Proyecto](#estructura-del-proyecto)
13. [Conclusiones sobre Paralelización](#conclusiones-sobre-paralelización)

---

## Requisitos del Sistema

### Software
- **Compilador C**: GCC o Clang con soporte para C11 (`stdatomic.h`)
- **Make**: GNU Make o compatible
- **Bash**: Para el script de benchmark (versión 3.2+)
- **bc**: Calculadora de línea de comandos (para el benchmark)
- **Python 3**: Para gantt_chart.py
- **matplotlib**: Para generación de diagramas de Gantt (`pip3 install matplotlib`)
- **LaTeX**: Opcional, para compilar informe.tex (`pdflatex`)

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

### Compilación

```bash
make all    # Compilar ambos programas
make clean  # Limpiar ejecutables y archivos .rle
```

### Ejecución básica (imagen sintética)

```bash
# Versión secuencial — genera imagen sintética 4096×4096 automáticamente
./rle_secuencial

# Versión paralela — misma imagen, múltiples hilos
./rle_paralelo
```

### Ejecución con imagen

```bash
# Con archivo PPM
./rle_secuencial foto.ppm
./rle_paralelo foto.ppm

# Con cualquier imagen (PNG, JPG, BMP)
./rle_secuencial foto.jpg
./rle_paralelo foto.jpg
```

### Script unificado (recomendado)

```bash
# Con selector de archivos (macOS)
./run_compresion.sh

# Con imagen específica
./run_compresion.sh imagen.jpg

# Genera automáticamente:
# - Compresión secuencial y paralela
# - Diagrama de Gantt (gantt_scheduling.png)
# - Resumen de archivos generados
```

### Benchmark automático (benchmark.sh)

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

### Generar diagrama de Gantt

```bash
# Los CSV se generan automáticamente al ejecutar rle_secuencial/rle_paralelo
python3 gantt_chart.py <imagen>_secuencial_gantt.csv <imagen>_paralelo_gantt.csv <output>.png
```

### Compilar informe

```bash
# Generar PDF del informe completo
pdflatex informe.tex
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
├── run_compresion.sh      # Script unificado con UI visual
├── gantt_chart.py         # Generador de diagramas de Gantt
├── stb_image.h           # Biblioteca para carga de imágenes
├── README.md              # Esta documentación
├── informe.tex           # Informe completo en LaTeX
├── captures/             # Capturas de pantalla del desarrollo
└── image/                # Imágenes de prueba y outputs
```

### Organización del código (rle_secuencial.c y rle_paralelo.c)

Cada archivo `.c` está organizado en secciones claramente delimitadas:

| Sección | Contenido |
|---------|-----------|
| 1. Includes y defines | Bibliotecas, constantes, macros de colores |
| 2. Estructuras de datos | `Image`, `Buffer`, `Progress`/`ThreadArg` |
| 3. Buffer dinámico | `buffer_init`, `buffer_push` con crecimiento amortizado |
| 4. Métricas del sistema | `get_rss`, `get_cpu_times`, `get_thread_cpu`, sampling de PC |
| 5. Lectura de imágenes | Parser PPM, carga con stb_image.h |
| 6. Imagen sintética | `generate_synthetic` — patrones de prueba deterministas |
| 7. Visualización | Barras de progreso, recuadros ANSI |
| 8. Hilo monitor | `monitor_func` — refresco periódico del display |
| 9. Compresión RLE | `rle_compress` / `rle_thread_func` |
| 10. CSV de scheduling | Generación de datos para gantt_chart.py |
| 11. Función main | Orquestación del flujo completo |

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
| PCI samples | `thread_info` | Muestreo del Program Counter |
| Scheduling CSV | Generado | Datos para diagrama de Gantt |

### Versión paralela (adicional)

| Métrica | Fuente | Descripción |
|---------|--------|-------------|
| TID por hilo | `pthread_threadid_np` | ID nativo del sistema operativo |
| Core asignado | `thread_affinity_policy` | Core donde ejecuta el hilo |
| CPU por hilo | Mach `thread_info` | ms de CPU consumidos individualmente |
| Progreso por hilo | `atomic_size_t` | Barra visual independiente por hilo |
| PCI por hilo | Muestreo individual | Evolución del PC de cada hilo |
| Speedup | Calculado | `cpu_total / wall_time` (>1 = paralelismo real) |
| Uso CPU % | Calculado | Puede superar 100% (N cores × 100%) |
| Scheduling CSV | Generado | Datos para gantt_chart.py |

### Archivos CSV de Scheduling

Los programas generan archivos CSV con datos de scheduling:

```
<imagen>_secuencial_gantt.csv
<imagen>_paralelo_gantt.csv
```

Contenido:
- Metadatos (wall time, CPU total, número de hilos)
- Información por hilo (TID, core, start/end time, CPU user/sys)
- Muestreo del Program Counter a ~1ms interval

---

## Análisis Comparativo

### Cómo demostrar el paralelismo

La **evidencia clave** de que se están usando múltiples cores es:

1. **CPU total > Wall time**: Si la suma de CPU de todos los hilos supera el tiempo real transcurrido, necesariamente están ejecutándose en paralelo.

2. **Uso de CPU > 100%**: Un valor de 400% significa que se están usando 4 cores simultáneamente.

3. **Progreso simultáneo**: En el display en tiempo real, todas las barras de progreso avanzan al mismo tiempo.

4. **Tiempos de CPU similares por hilo**: Si todos los hilos consumen ~2-3ms de CPU, están ejecutándose en paralelo, no secuencialmente.

5. **Diagrama de Gantt**: Las barras de CPU de cada hilo son sólidas y simultáneas, no escalonadas.

6. **Evolución del PCI**: Múltiples líneas de PC evolucionan simultáneamente.

### Métricas típicas observadas

| Métrica | Secuencial | Paralelo (8 cores) |
|---------|-----------|---------------------|
| Wall time | ~23 ms | ~5.6 ms |
| CPU total | ~24 ms | ~30 ms |
| Uso CPU | ~100% | ~530% |
| Speedup | 1.0x | ~4.1x |
| Memoria | ~50 MB | ~50 MB |
| Resultado | 264,200 bytes | 264,200 bytes (idéntico) |

### Diagnóstico: Speedup Negativo

El proyecto incluye un análisis detallado de qué ocurre cuando el speedup es < 1 (paralelo más lento que secuencial):

**Causas comunes:**
- `usleep()` artificial entre `pthread_create()` — hilos escalonados, no paralelos
- Contadores atómicos en hot loops — cache-line bouncing
- Datos insuficientes por hilo — overhead > beneficio

**Soluciones:**
- Eliminar pausas innecesarias entre creación de hilos
- Mover atómicos fuera de bucles críticos
- Usar buffers locales, contador local + atómico al final

Este análisis está documentado en `informe.tex` con capturas de Gantt mostrando el problema y la solución.

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

### Entrada: Otros formatos (PNG, JPG, BMP)

El programa utiliza `stb_image.h` para cargar cualquier formato de imagen estándar. La imagen se convierte a RGB de 8 bits.

### Salida: .rle

```
Offset 0:   uint32_t width      (4 bytes, little-endian en x86)
Offset 4:   uint32_t height     (4 bytes)
Offset 8+:  runs RLE            (secuencia de bloques de 4 bytes)
              [count][R][G][B]   count: 1-255
              [count][R][G][B]
              ...
```

### Salida: CSV de Scheduling

```
<meta>
wall_ms,total_cpu_ms,num_threads,pixels,pid

<threads>
thread_id,tid,start_ms,end_ms,cpu_user_ms,cpu_sys_ms,pixels,stack_addr

<pc_samples>
timestamp_ms,thread_id,pc_addr,pixels_at
```

### Salida: Diagrama de Gantt (PNG)

Imagen generada por `gantt_chart.py` con múltiples paneles:
- Diagrama principal Secuencial vs Paralelo
- Asignación de CPU por el Scheduler
- Evolución del Program Counter
- Progreso de compresión por hilo
- Resumen de conceptos de SO

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

---

## Herramientas de Análisis

### run_compresion.sh — Script Unificado con Visualización

Script Bash con interfaz visual completa que:

- Abre explorador de archivos para seleccionar imagen (macOS)
- Ejecuta versión secuencial y paralela automáticamente
- Genera diagrama de Gantt con `gantt_chart.py`
- Muestra resumen de archivos generados
- Abre el diagrama automáticamente

```bash
# Ejecutar con selector de archivos
./run_compresion.sh

# O pasar imagen directamente
./run_compresion.sh imagen.jpg
```

### gantt_chart.py — Generador de Diagramas de Gantt

Visualización completa de la planificación de hilos usando matplotlib:

- **Diagrama principal**: Secuencial vs Paralelo con barras de CPU vs Wall time
- **Asignación de CPU**: Cómo el scheduler distribuye hilos en cores
- **Evolución del PCI**: Program Counter de cada hilo en el tiempo
- **Progreso por hilo**: Curvas de compresión individuales
- **Conceptos SO**: Resumen de PCB/TCB, contextos, hilos, scheduler

```bash
python3 gantt_chart.py <csv_secuencial> <csv_paralelo> <output.png>
```

Archivos CSV generados automaticamente por los programas RLE con datos de scheduling.

### informe.tex — Documentación Completa del Proyecto

Informe formal en LaTeX (~1500+ líneas) que incluye:

- Metodología de desarrollo asistido por IA (Claude Code)
- Iteraciones del proyecto con capturas de pantalla
- Análisis técnico del código fuente
- Análisis comparativo de rendimiento
- Diagnóstico de speedup negativo y optimizaciones
- Visualización de segmentos de memoria (PILA, DATA, BSS, HEAP)
- Diagramas de Gantt generados

Compilar con:
```bash
pdflatex informe.tex
```

---

## Conceptos de SO Demostrados

Este proyecto es una herramienta pedagógica para demostrar conceptos fundamentales de sistemas operativos:

| Concepto | Descripción | Cómo se demuestra |
|----------|-------------|------------------|
| **Hilos (Threads)** | Unidad básica de ejecución | pthread_create/pthread_join, TIDs únicos |
| **Scheduler/Planificador** | Asigna CPU a hilos | Distribución automática en cores, affinity policy |
| **PCB / TCB** | Bloques de control de proceso/hilo | TID, PC, stack, estado por hilo |
| **PCI (Program Counter)** | Dirección de instrucción actual | Evolución del PC en cada hilo |
| **Cambio de Contexto** | Guardado/restaurado de estado | Context switches detectados en Gantt |
| **Concurrencia vs Paralelismo** | Múltiples hilos, ejecución simultánea | Speedup > 1 demuestra paralelismo real |
| **Variables Atómicas** | Lock-free communication | atomic_store/load para progreso |
| **Memoria Compartida vs Privada** | Datos comunes vs locales | Imagen compartida (RO), buffers privados |
| **Sincronización** | Coordinación entre hilos | Ausencia de mutex (datos disjuntos) |
| **Ley de Amdahl** | Límite del speedup | Speedup < N cores por fracción serial |
| **Overhead de Paralelización** | Costo de crear/gestionar hilos | Análisis de speedup negativo y optimizaciones |

---

## Estructura del Proyecto

```
SO/
├── rle_secuencial.c       # Versión secuencial (1 hilo, ~750 líneas)
├── rle_paralelo.c         # Versión paralela (N hilos, ~850 líneas)
├── Makefile               # Sistema de compilación
├── benchmark.sh           # Script de análisis comparativo
├── run_compresion.sh      # Script unificado con UI visual
├── gantt_chart.py         # Generador de diagramas de Gantt
├── stb_image.h           # Biblioteca para carga de imágenes
├── README.md              # Esta documentación
├── informe.tex           # Informe completo en LaTeX
├── captures/             # Capturas de pantalla del desarrollo
│   ├── cap_00.png - cap_45.png
│   └── cap_gantt_scheduling.png
└── image/                # Imágenes de prueba y outputs
    ├── test-0.jpg, test-1.jpg, test-2.jpg
    ├── test-africa.png, unsaac.jpg
    ├── *_secuencial.rle / *_paralelo.rle
    ├── *_secuencial_descomprimida.bmp / *_paralelo_descomprimida.bmp
    ├── *_secuencial_gantt.csv / *_paralelo_gantt.csv
    └── *_gantt_scheduling.png
```

### Archivos Generados por los Programas

| Extensión | Descripción |
|-----------|-------------|
| `.rle` | Archivo comprimido con algoritmo RLE |
| `_descomprimida.bmp` | Imagen descomprimida para verificación |
| `_gantt.csv` | Datos de scheduling para gantt_chart.py |
| `_gantt_scheduling.png` | Diagrama de Gantt visual |
| `.raw` | Datos crudos en escala de grises |

---