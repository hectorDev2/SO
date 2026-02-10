#!/bin/bash
# ============================================================================
#  run_compresion.sh — Script unificado de Compresión RLE
#                     Curso de Sistemas Operativos
#
#  1. Abre el explorador de archivos para seleccionar una imagen
#  2. Ejecuta el algoritmo SECUENCIAL (1 hilo)
#  3. Ejecuta el algoritmo PARALELO (N hilos)
#  4. Genera diagrama de Gantt con la planificación de hilos
#  5. Muestra resumen comparativo
# ============================================================================

CYAN='\033[1;36m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
RED='\033[1;31m'
WHITE='\033[1;37m'
MAGENTA='\033[1;35m'
RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ─── Verificar que los binarios existen ───
if [ ! -f "./rle_secuencial" ] || [ ! -f "./rle_paralelo" ]; then
    echo -e "${YELLOW}Compilando programas...${RESET}"
    gcc -o rle_secuencial rle_secuencial.c -lpthread -lm 2>&1
    gcc -o rle_paralelo rle_paralelo.c -lpthread -lm 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}Error de compilación.${RESET}"
        exit 1
    fi
    echo -e "${GREEN}Compilación exitosa.${RESET}"
fi

# ─── Banner ───
echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════════════╗${RESET}"
echo -e "${CYAN}║${RESET}                                                                      ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}██████╗ ██╗     ███████╗     ██████╗ ██████╗ ███╗   ███╗██████╗${RESET}   ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}██╔══██╗██║     ██╔════╝    ██╔════╝██╔═══██╗████╗ ████║██╔══██╗${RESET}  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}██████╔╝██║     █████╗      ██║     ██║   ██║██╔████╔██║██████╔╝${RESET}  ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}██╔══██╗██║     ██╔══╝      ██║     ██║   ██║██║╚██╔╝██║██╔═══╝${RESET}   ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}██║  ██║███████╗███████╗    ╚██████╗╚██████╔╝██║ ╚═╝ ██║██║${RESET}       ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${YELLOW}╚═╝  ╚═╝╚══════╝╚══════╝     ╚═════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝${RESET}       ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}                                                                      ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${WHITE}Compresión RLE — Análisis de Scheduling del SO${RESET}                     ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}  ${WHITE}Secuencial vs Paralelo con Diagrama de Gantt${RESET}                      ${CYAN}║${RESET}"
echo -e "${CYAN}║${RESET}                                                                      ${CYAN}║${RESET}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════════════╝${RESET}"
echo ""

# ─── Seleccionar imagen ───
if [ -n "$1" ]; then
    IMAGE_PATH="$1"
    echo -e "  ${GREEN}Imagen recibida por argumento:${RESET} $IMAGE_PATH"
else
    echo -e "  ${WHITE}Abriendo explorador de archivos...${RESET}"
    echo -e "  ${YELLOW}Selecciona una imagen (PNG, JPG, BMP, GIF, etc.)${RESET}"
    echo ""

    IMAGE_PATH=$(osascript -e 'set theFile to choose file with prompt "Seleccione una imagen para comprimir (RLE)" of type {"public.png", "public.jpeg", "public.bmp", "public.gif", "public.targa-image", "com.adobe.photoshop-image", "public.radiance"}' -e 'POSIX path of theFile' 2>/dev/null)

    if [ -z "$IMAGE_PATH" ]; then
        echo -e "  ${RED}No se seleccionó ningún archivo. Saliendo.${RESET}"
        exit 1
    fi
fi

echo ""
echo -e "  ${GREEN}Imagen seleccionada:${RESET} $IMAGE_PATH"
echo ""

# Nombre base para archivos de salida
BASENAME=$(basename "$IMAGE_PATH" | sed 's/\.[^.]*$//')
OUTDIR="$(dirname "$IMAGE_PATH")"

echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo -e "  ${YELLOW}[PASO 1/4]${RESET}  Ejecutando compresión ${RED}SECUENCIAL${RESET} (1 hilo)..."
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo ""

./rle_secuencial "$IMAGE_PATH"

echo ""
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo -e "  ${YELLOW}[PASO 2/4]${RESET}  Ejecutando compresión ${GREEN}PARALELA${RESET} (N hilos)..."
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo ""

./rle_paralelo "$IMAGE_PATH"

# ─── Buscar los CSV generados ───
CSV_SEQ="${IMAGE_PATH}_secuencial_gantt.csv"
CSV_PAR="${IMAGE_PATH}_paralelo_gantt.csv"
GANTT_PNG="${IMAGE_PATH}_gantt_scheduling.png"

echo ""
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo -e "  ${YELLOW}[PASO 3/4]${RESET}  Generando ${MAGENTA}Diagrama de Gantt${RESET}..."
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo ""

if [ -f "$CSV_SEQ" ] && [ -f "$CSV_PAR" ]; then
    python3 "$SCRIPT_DIR/gantt_chart.py" "$CSV_SEQ" "$CSV_PAR" "$GANTT_PNG"

    if [ $? -eq 0 ] && [ -f "$GANTT_PNG" ]; then
        echo -e "  ${GREEN}Diagrama generado exitosamente.${RESET}"
    else
        echo -e "  ${RED}Error al generar el diagrama.${RESET}"
        echo -e "  ${YELLOW}Asegúrate de tener matplotlib: pip3 install matplotlib${RESET}"
    fi
else
    echo -e "  ${RED}No se encontraron los archivos CSV de scheduling.${RESET}"
fi

# ─── Resumen ───
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════════════${RESET}"
echo -e "  ${YELLOW}[PASO 4/4]${RESET}  ${WHITE}Resumen de archivos generados${RESET}"
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════════════${RESET}"
echo ""

echo -e "  ${WHITE}Archivos del algoritmo SECUENCIAL:${RESET}"
[ -f "${IMAGE_PATH}_secuencial.rle" ] && echo -e "    ${GREEN}[RLE]${RESET} ${IMAGE_PATH}_secuencial.rle"
[ -f "${IMAGE_PATH}_secuencial_descomprimida.bmp" ] && echo -e "    ${GREEN}[BMP]${RESET} ${IMAGE_PATH}_secuencial_descomprimida.bmp"
[ -f "${IMAGE_PATH}.raw" ] && echo -e "    ${CYAN}[RAW]${RESET} ${IMAGE_PATH}.raw (grayscale)"
[ -f "$CSV_SEQ" ] && echo -e "    ${GREEN}[CSV]${RESET} $CSV_SEQ"
echo ""

echo -e "  ${WHITE}Archivos del algoritmo PARALELO:${RESET}"
[ -f "${IMAGE_PATH}_paralelo.rle" ] && echo -e "    ${GREEN}[RLE]${RESET} ${IMAGE_PATH}_paralelo.rle"
[ -f "${IMAGE_PATH}_paralelo_descomprimida.bmp" ] && echo -e "    ${GREEN}[BMP]${RESET} ${IMAGE_PATH}_paralelo_descomprimida.bmp"
[ -f "$CSV_PAR" ] && echo -e "    ${GREEN}[CSV]${RESET} $CSV_PAR"
echo ""

echo -e "  ${WHITE}Archivos del algoritmo SECUENCIAL:${RESET}"
[ -f "${IMAGE_PATH}_secuencial.rle" ] && echo -e "    ${GREEN}[RLE]${RESET} ${IMAGE_PATH}_secuencial.rle"
[ -f "${IMAGE_PATH}_secuencial_descomprimida.bmp" ] && echo -e "    ${GREEN}[BMP]${RESET} ${IMAGE_PATH}_secuencial_descomprimida.bmp"
[ -f "$CSV_SEQ" ] && echo -e "    ${GREEN}[CSV]${RESET} $CSV_SEQ"
echo ""

echo -e "  ${WHITE}Archivos del algoritmo PARALELO:${RESET}"
[ -f "${IMAGE_PATH}_paralelo.rle" ] && echo -e "    ${GREEN}[RLE]${RESET} ${IMAGE_PATH}_paralelo.rle"
[ -f "${IMAGE_PATH}_paralelo_descomprimida.bmp" ] && echo -e "    ${GREEN}[BMP]${RESET} ${IMAGE_PATH}_paralelo_descomprimida.bmp"
[ -f "$CSV_PAR" ] && echo -e "    ${GREEN}[CSV]${RESET} $CSV_PAR"
echo ""

echo -e "  ${WHITE}Diagrama de Gantt:${RESET}"
[ -f "$GANTT_PNG" ] && echo -e "    ${MAGENTA}[PNG]${RESET} $GANTT_PNG"
echo ""

# ─── Abrir el diagrama de Gantt ───
if [ -f "$GANTT_PNG" ]; then
    echo -e "  ${YELLOW}Abriendo diagrama de Gantt...${RESET}"
    open "$GANTT_PNG"
fi

echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo -e "  ${GREEN}Proceso completado.${RESET}"
echo -e "${CYAN}══════════════════════════════════════════════════════════════════════${RESET}"
echo ""
