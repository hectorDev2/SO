# ============================================================================
#  Makefile — Compresión RLE de Imágenes: Secuencial vs Paralelo
# ============================================================================
#
#  Targets:
#    make all          Compila ambos programas
#    make secuencial   Solo la versión secuencial
#    make paralelo     Solo la versión paralela
#    make benchmark    Compila y ejecuta el análisis comparativo
#    make clean        Elimina ejecutables y archivos .rle
#
# ============================================================================

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread

# ─── Targets principales ───

all: rle_secuencial rle_paralelo

rle_secuencial: rle_secuencial.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rle_paralelo: rle_paralelo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ─── Aliases ───

secuencial: rle_secuencial

paralelo: rle_paralelo

# ─── Benchmark ───

benchmark: all
	@chmod +x benchmark.sh
	@./benchmark.sh

# ─── Limpieza ───

clean:
	rm -f rle_secuencial rle_paralelo *.rle reporte_analisis.txt

.PHONY: all clean secuencial paralelo benchmark
