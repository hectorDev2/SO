#!/bin/bash
# ============================================================================
#  benchmark.sh — Análisis Comparativo: Secuencial vs Paralelo
# ============================================================================
#
#  Ejecuta ambas versiones múltiples veces, recolecta métricas y genera
#  un reporte comparativo detallado.
#
#  USO:
#      chmod +x benchmark.sh
#      ./benchmark.sh              # 5 iteraciones (por defecto)
#      ./benchmark.sh 10           # 10 iteraciones
#      ./benchmark.sh 5 imagen.ppm # 5 iteraciones con archivo PPM
# ============================================================================

set -e

ITERACIONES=${1:-5}
INPUT_FILE=${2:-""}
REPORTE="reporte_analisis.txt"

# Colores
CYAN='\033[1;36m'
YELLOW='\033[1;33m'
GREEN='\033[1;32m'
RED='\033[1;31m'
BOLD='\033[1m'
RESET='\033[0m'

# ─── Verificar que los binarios existen ───
if [ ! -f "./rle_secuencial" ] || [ ! -f "./rle_paralelo" ]; then
    echo -e "${RED}Error: Los binarios no existen. Ejecute 'make all' primero.${RESET}"
    exit 1
fi

# ─── Detectar información del sistema ───
OS_NAME=$(uname -s)
OS_VERSION=$(uname -r)
CPU_MODEL=""
NUM_CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo "?")

if [ "$OS_NAME" = "Darwin" ]; then
    CPU_MODEL=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "Desconocido")
    RAM_TOTAL=$(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.1f GB", $1/1073741824}')
else
    CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo "Desconocido")
    RAM_TOTAL=$(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo "?")
fi

echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════╗${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}BENCHMARK: Compresión RLE — Secuencial vs Paralelo${RESET}            ${CYAN}║${RESET}"
echo -e "${CYAN}╠══════════════════════════════════════════════════════════════════╣${RESET}"
echo -e "${CYAN}║${RESET}  Sistema:     $OS_NAME $OS_VERSION"
echo -e "${CYAN}║${RESET}  CPU:         $CPU_MODEL"
echo -e "${CYAN}║${RESET}  Cores:       $NUM_CORES"
echo -e "${CYAN}║${RESET}  RAM:         $RAM_TOTAL"
echo -e "${CYAN}║${RESET}  Iteraciones: $ITERACIONES"
if [ -n "$INPUT_FILE" ]; then
    echo -e "${CYAN}║${RESET}  Entrada:     $INPUT_FILE"
else
    echo -e "${CYAN}║${RESET}  Entrada:     Imagen sintética 4096x4096"
fi
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════╝${RESET}"
echo ""

# ─── Función para extraer métricas de la salida del programa ───
strip_ansi() {
    # Eliminar secuencias de escape ANSI para parseo limpio
    echo "$1" | sed 's/\x1b\[[0-9;]*m//g'
}

extract_time() {
    strip_ansi "$1" | grep "Tiempo total:" | grep -oE '[0-9]+\.[0-9]+' | head -1
}

extract_compressed() {
    strip_ansi "$1" | grep "comprimido:" | grep -oE '[0-9]+' | head -1
}

extract_ratio() {
    strip_ansi "$1" | grep "Ratio" | grep -oE '[0-9]+\.[0-9]+' | head -1
}

extract_threads() {
    strip_ansi "$1" | grep "Hilos utilizados:" | grep -oE '[0-9]+' | head -1
}

# ─── Arrays para acumular resultados ───
declare -a SEQ_TIMES
declare -a PAR_TIMES

# ─── Construir argumentos ───
ARGS=""
if [ -n "$INPUT_FILE" ]; then
    ARGS="$INPUT_FILE"
fi

# ─── Ejecutar benchmarks ───
echo -e "${BOLD}Ejecutando benchmarks...${RESET}"
echo ""

for i in $(seq 1 "$ITERACIONES"); do
    printf "  Iteración %d/%d: " "$i" "$ITERACIONES"

    # Secuencial
    SEQ_OUTPUT=$(./rle_secuencial $ARGS 2>&1)
    SEQ_T=$(extract_time "$SEQ_OUTPUT")
    SEQ_TIMES+=("$SEQ_T")

    # Paralelo
    PAR_OUTPUT=$(./rle_paralelo $ARGS 2>&1)
    PAR_T=$(extract_time "$PAR_OUTPUT")
    PAR_TIMES+=("$PAR_T")

    ITER_SPEEDUP=$(echo "scale=2; $SEQ_T / $PAR_T" | bc 2>/dev/null || echo "N/A")
    printf "Seq=%.6fs  Par=%.6fs  Speedup=${GREEN}%sx${RESET}\n" "$SEQ_T" "$PAR_T" "$ITER_SPEEDUP"
done

# ─── Extraer métricas constantes de la última ejecución ───
COMPRESSED=$(extract_compressed "$SEQ_OUTPUT")
RATIO=$(extract_ratio "$SEQ_OUTPUT")
THREADS=$(extract_threads "$PAR_OUTPUT")

# ─── Calcular estadísticas ───
# Nota: Bash 3.2 (macOS) no soporta namerefs (local -n), así que
# calculamos las estadísticas directamente sobre cada array.

SEQ_SUM=0; SEQ_MIN=999999; SEQ_MAX=0
for val in "${SEQ_TIMES[@]}"; do
    SEQ_SUM=$(echo "$SEQ_SUM + $val" | bc)
    if (( $(echo "$val < $SEQ_MIN" | bc -l) )); then SEQ_MIN=$val; fi
    if (( $(echo "$val > $SEQ_MAX" | bc -l) )); then SEQ_MAX=$val; fi
done
SEQ_AVG=$(echo "scale=6; $SEQ_SUM / $ITERACIONES" | bc)

PAR_SUM=0; PAR_MIN=999999; PAR_MAX=0
for val in "${PAR_TIMES[@]}"; do
    PAR_SUM=$(echo "$PAR_SUM + $val" | bc)
    if (( $(echo "$val < $PAR_MIN" | bc -l) )); then PAR_MIN=$val; fi
    if (( $(echo "$val > $PAR_MAX" | bc -l) )); then PAR_MAX=$val; fi
done
PAR_AVG=$(echo "scale=6; $PAR_SUM / $ITERACIONES" | bc)

SPEEDUP_AVG=$(echo "scale=2; $SEQ_AVG / $PAR_AVG" | bc)
SPEEDUP_MIN=$(echo "scale=2; $SEQ_MIN / $PAR_MAX" | bc)
SPEEDUP_MAX=$(echo "scale=2; $SEQ_MAX / $PAR_MIN" | bc)
EFICIENCIA=$(echo "scale=1; $SPEEDUP_AVG / $THREADS * 100" | bc)

# ─── Verificar archivos iguales ───
FILES_MATCH="NO"
if diff output_secuencial.rle output_paralelo.rle > /dev/null 2>&1; then
    FILES_MATCH="SÍ"
fi

# ─── Mostrar resultados ───
echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════╗${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}RESULTADOS DEL ANÁLISIS COMPARATIVO${RESET}                            ${CYAN}║${RESET}"
echo -e "${CYAN}╠══════════════════════════════════════════════════════════════════╣${RESET}"
echo -e "${CYAN}║${RESET}                                                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${BOLD}VERSIÓN SECUENCIAL (1 hilo):${RESET}                                   ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo promedio:  ${BOLD}${SEQ_AVG}${RESET} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo mínimo:    ${SEQ_MIN} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo máximo:    ${SEQ_MAX} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}                                                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${BOLD}VERSIÓN PARALELA (${THREADS} hilos):${RESET}                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo promedio:  ${BOLD}${PAR_AVG}${RESET} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo mínimo:    ${PAR_MIN} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tiempo máximo:    ${PAR_MAX} s                               ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}                                                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${BOLD}COMPARACIÓN:${RESET}                                                   ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Speedup promedio: ${GREEN}${BOLD}${SPEEDUP_AVG}x${RESET}                                         ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Speedup rango:    ${SPEEDUP_MIN}x — ${SPEEDUP_MAX}x                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Eficiencia:       ${EFICIENCIA}% (speedup / hilos × 100)           ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Archivos iguales: ${FILES_MATCH} (verificación de corrección)         ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}                                                                  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${BOLD}COMPRESIÓN:${RESET}                                                    ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Tamaño comprimido: ${COMPRESSED} bytes                            ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}    Ratio:             ${RATIO}%                                      ${CYAN}║${RESET}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════╝${RESET}"

# ─── Generar reporte en texto plano ───
cat > "$REPORTE" << REPORT_EOF
================================================================================
 REPORTE DE ANÁLISIS COMPARATIVO
 Compresión RLE de Imágenes: Secuencial vs Paralelo
================================================================================

 Fecha:         $(date '+%Y-%m-%d %H:%M:%S')
 Sistema:       $OS_NAME $OS_VERSION
 CPU:           $CPU_MODEL
 Cores:         $NUM_CORES
 RAM:           $RAM_TOTAL
 Iteraciones:   $ITERACIONES

================================================================================
 1. CONFIGURACIÓN DEL EXPERIMENTO
================================================================================

 Imagen de prueba:
   - Tipo:       $([ -n "$INPUT_FILE" ] && echo "Archivo PPM ($INPUT_FILE)" || echo "Sintética (bandas horizontales)")
   - Dimensiones: 4096 x 4096 píxeles
   - Tamaño:     50,331,648 bytes (48 MB)
   - Píxeles:    16,777,216

 Algoritmo:     Run-Length Encoding (RLE) sobre píxeles RGB
 Formato run:   [count: 1B][R: 1B][G: 1B][B: 1B] = 4 bytes por run

 Versión secuencial: 1 hilo de compresión
 Versión paralela:   $THREADS hilos de compresión (1 por core)

================================================================================
 2. RESULTADOS DE EJECUCIÓN
================================================================================

 Tiempos individuales por iteración:

 Iter    Secuencial (s)    Paralelo (s)    Speedup
 ────    ──────────────    ────────────    ───────
REPORT_EOF

for i in $(seq 0 $((ITERACIONES - 1))); do
    iter_speedup=$(echo "scale=2; ${SEQ_TIMES[$i]} / ${PAR_TIMES[$i]}" | bc 2>/dev/null || echo "N/A")
    printf " %3d     %s          %s         %sx\n" $((i + 1)) "${SEQ_TIMES[$i]}" "${PAR_TIMES[$i]}" "$iter_speedup" >> "$REPORTE"
done

cat >> "$REPORTE" << REPORT_EOF

 Estadísticas:

                    Secuencial       Paralelo
                    ──────────       ────────
 Promedio:          ${SEQ_AVG} s     ${PAR_AVG} s
 Mínimo:            ${SEQ_MIN} s     ${PAR_MIN} s
 Máximo:            ${SEQ_MAX} s     ${PAR_MAX} s

================================================================================
 3. ANÁLISIS DE RENDIMIENTO
================================================================================

 Speedup promedio:     ${SPEEDUP_AVG}x
 Speedup rango:        ${SPEEDUP_MIN}x — ${SPEEDUP_MAX}x
 Eficiencia paralela:  ${EFICIENCIA}%
 Hilos utilizados:     ${THREADS}
 Speedup teórico max:  ${THREADS}.00x (Ley de Amdahl, parte serial ≈ 0)

 Verificación de corrección:
   Los archivos .rle producidos por ambas versiones son: ${FILES_MATCH} idénticos

 Compresión:
   Tamaño original:    50,331,648 bytes
   Tamaño comprimido:  ${COMPRESSED} bytes
   Ratio:              ${RATIO}%

================================================================================
 4. ANÁLISIS DE USO DE RECURSOS
================================================================================

 Versión Secuencial:
   - Usa 1 core durante toda la ejecución
   - Uso de CPU esperado: ~100% (limitado a 1 core)
   - Memoria: ~50 MB (imagen) + buffer de compresión
   - No tiene overhead de creación de hilos ni sincronización

 Versión Paralela:
   - Usa $THREADS cores simultáneamente
   - Uso de CPU esperado: hasta ${THREADS}00% (${THREADS} cores × 100%)
   - Memoria: ~50 MB (imagen compartida) + $THREADS buffers de compresión
   - Overhead mínimo: solo variables atómicas para progreso (lock-free)
   - Sin mutex, sin semáforos, sin condiciones de carrera

 La prueba definitiva de paralelismo real es:
   CPU_total > wall_time  →  múltiples cores activos simultáneamente
   En la versión paralela: Speedup CPU ≈ ${SPEEDUP_AVG}x confirma uso de ~${SPEEDUP_AVG} cores

================================================================================
 5. CONCLUSIONES
================================================================================

 5.1. Cuándo usar paralelización:

   RECOMENDADO cuando:
   ✓ El problema es "embarrassingly parallel" (divisible sin dependencias)
   ✓ El volumen de datos es grande (>1 MB de procesamiento por hilo)
   ✓ Cada hilo puede trabajar en memoria independiente
   ✓ La proporción serial del algoritmo es pequeña
   ✓ Se necesita reducir latencia en operaciones CPU-bound

   NO RECOMENDADO cuando:
   ✗ El dataset es pequeño (overhead > beneficio)
   ✗ La tarea es I/O-bound (el cuello de botella no es la CPU)
   ✗ Hay muchas dependencias de datos entre secciones
   ✗ La sincronización requerida es compleja (locks, barreras)
   ✗ La portabilidad es más importante que el rendimiento

 5.2. Factores que limitan el speedup:

   - Ley de Amdahl: la porción serial limita el speedup máximo
   - Overhead de creación de hilos (~microsegundos por hilo)
   - Cache effects: cada hilo accede a diferentes regiones de memoria
   - NUMA effects: en sistemas multi-socket, acceso a memoria remota
   - OS scheduling: el kernel puede no distribuir óptimamente
   - False sharing: si las variables atómicas están en la misma cache line

 5.3. Resultado específico de este benchmark:

   Con $THREADS cores disponibles, se obtuvo un speedup de ${SPEEDUP_AVG}x,
   lo que representa una eficiencia del ${EFICIENCIA}%.
   La compresión RLE es altamente paralelizable porque cada bloque de filas
   se comprime de forma completamente independiente.

================================================================================
 Fin del reporte
================================================================================
REPORT_EOF

echo ""
echo -e "${GREEN}Reporte guardado en: ${BOLD}${REPORTE}${RESET}"

# ─── Limpiar archivos temporales ───
rm -f output_secuencial.rle output_paralelo.rle
