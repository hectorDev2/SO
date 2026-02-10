#!/usr/bin/env python3
"""
gantt_chart.py — Diagrama de Gantt: Planificación de Hilos y Gestión del SO
               Curso de Sistemas Operativos - UNSAAC

Uso: python3 gantt_chart.py <csv_secuencial> <csv_paralelo> <output.png>
"""

import sys
import os

def parse_csv(filepath):
    data = {'meta': {}, 'threads': [], 'pc_samples': {}}
    with open(filepath, 'r') as f:
        content = f.read().strip()
    sections = content.split('\n\n')

    lines = sections[0].strip().split('\n')
    keys = lines[0].split(',')
    vals = lines[1].split(',')
    for k, v in zip(keys, vals):
        data['meta'][k.strip()] = v.strip()

    lines = sections[1].strip().split('\n')
    header = lines[0].split(',')
    for line in lines[1:]:
        vals = line.split(',')
        thread = {}
        for k, v in zip(header, vals):
            thread[k.strip()] = v.strip()
        data['threads'].append(thread)

    if len(sections) > 2:
        lines = sections[2].strip().split('\n')
        header = lines[0].split(',')
        for line in lines[1:]:
            vals = line.split(',')
            sample = {}
            for k, v in zip(header, vals):
                sample[k.strip()] = v.strip()
            tid = int(sample['thread_id'])
            if tid not in data['pc_samples']:
                data['pc_samples'][tid] = []
            data['pc_samples'][tid].append(sample)
    return data


def generate_gantt(seq_csv, par_csv, output_path):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        import matplotlib.patches as mpatches
        import matplotlib.gridspec as gridspec
        from matplotlib.patches import FancyBboxPatch
    except ImportError:
        print("Error: pip3 install matplotlib")
        sys.exit(1)

    seq = parse_csv(seq_csv)
    par = parse_csv(par_csv)

    nt = int(par['meta']['num_threads'])
    wall_seq = float(seq['meta']['wall_ms'])
    wall_par = float(par['meta']['wall_ms'])
    cpu_seq = float(seq['meta']['total_cpu_ms'])
    cpu_par = float(par['meta']['total_cpu_ms'])
    speedup = wall_seq / wall_par if wall_par > 0 else 0
    efficiency = speedup / nt * 100

    TC = [
        '#E74C3C', '#2ECC71', '#3498DB', '#F39C12',
        '#9B59B6', '#1ABC9C', '#E67E22', '#95A5A6',
        '#C0392B', '#27AE60', '#2980B9', '#D35400',
        '#8E44AD', '#16A085', '#F1C40F', '#7F8C8D'
    ]

    # ═══════════════════════════════════════════════
    #  LAYOUT: 5 filas x 2 columnas
    # ═══════════════════════════════════════════════
    fig = plt.figure(figsize=(22, 28))
    fig.patch.set_facecolor('#0d1117')

    gs = gridspec.GridSpec(5, 2, height_ratios=[2.2, 2.5, 2.5, 2.5, 1.5],
                           hspace=0.32, wspace=0.22,
                           left=0.06, right=0.97, top=0.955, bottom=0.025)

    T = '#e6edf3'   # text color
    G = '#21262d'    # grid
    BG = '#0d1117'   # background
    ABG = '#161b22'  # axes background
    ACC = '#F39C12'  # accent

    fig.text(0.5, 0.98,
             'DIAGRAMA DE GANTT — Planificación de Hilos y Gestión de Recursos del SO',
             ha='center', va='center', color=ACC, fontsize=18, fontweight='bold')
    fig.text(0.5, 0.968,
             'Compresión RLE: Análisis Secuencial vs Paralelo  |  Curso de Sistemas Operativos',
             ha='center', va='center', color='#8b949e', fontsize=11)

    # ═════════════════════════════════════════════════════════════
    #  FILA 1: DIAGRAMA DE GANTT — Secuencial (izq) vs Paralelo (der)
    # ═════════════════════════════════════════════════════════════

    # --- 1A: GANTT SECUENCIAL ---
    ax1a = fig.add_subplot(gs[0, 0])
    ax1a.set_facecolor(ABG)
    ax1a.set_title('SECUENCIAL — 1 Hilo de Ejecución',
                    color='#E74C3C', fontsize=12, fontweight='bold', pad=10)

    ax1a.barh(0, wall_seq, left=0, height=0.5,
              color='#E74C3C', alpha=0.9, edgecolor='white', linewidth=0.8)
    ax1a.text(wall_seq / 2, 0, f'Hilo Principal (PID {seq["meta"]["pid"]})\n{wall_seq:.2f} ms',
              ha='center', va='center', color='white', fontsize=9, fontweight='bold')

    # Anotaciones de tiempos CPU
    cpu_u = float(seq['threads'][0]['cpu_user_ms'])
    cpu_s = float(seq['threads'][0]['cpu_sys_ms'])
    ax1a.text(wall_seq + wall_seq * 0.02, 0,
              f'CPU user: {cpu_u:.1f} ms\nCPU sys:  {cpu_s:.1f} ms\n1 core usado',
              va='center', color='#8b949e', fontsize=8, fontfamily='monospace')

    ax1a.set_xlim(-wall_seq * 0.02, wall_seq * 1.4)
    ax1a.set_ylim(-0.8, 0.8)
    ax1a.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax1a.set_yticks([0])
    ax1a.set_yticklabels(['Hilo 0\n(main)'], color=T, fontsize=9)
    ax1a.tick_params(colors=T)
    ax1a.grid(axis='x', color=G, alpha=0.4, linestyle='--')
    for s in ['top', 'right']:
        ax1a.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax1a.spines[s].set_color(G)

    # --- 1B: GANTT PARALELO ---
    ax1b = fig.add_subplot(gs[0, 1])
    ax1b.set_facecolor(ABG)
    ax1b.set_title(f'PARALELO — {nt} Hilos de Ejecución',
                    color='#2ECC71', fontsize=12, fontweight='bold', pad=10)

    for t in par['threads']:
        tid = int(t['thread_id'])
        start = float(t['start_ms'])
        end = float(t['end_ms'])
        cpu_u = float(t['cpu_user_ms'])
        duration = end - start
        c = TC[tid % len(TC)]
        y = nt - tid

        # Wall time (transparente)
        ax1b.barh(y, duration, left=start, height=0.65,
                  color=c, alpha=0.15, edgecolor=c, linewidth=0.5)
        # CPU time (sólido)
        ax1b.barh(y, cpu_u, left=start, height=0.65,
                  color=c, alpha=0.85, edgecolor='white', linewidth=0.5)
        ax1b.text(start + duration / 2, y,
                  f'H-{tid}  {cpu_u:.1f}ms',
                  ha='center', va='center', color='white', fontsize=7, fontweight='bold')

    # Padre (join)
    ax1b.barh(0, wall_par, left=0, height=0.35,
              color=ACC, alpha=0.4, edgecolor=ACC, linewidth=0.5)
    ax1b.text(wall_par / 2, 0, f'Padre (pthread_join) — {wall_par:.1f} ms',
              ha='center', va='center', color=ACC, fontsize=7, fontweight='bold')

    ax1b.set_xlim(-wall_par * 0.02, wall_par * 1.1)
    ax1b.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    yticks = list(range(0, nt + 1))
    ylabels = ['Padre'] + [f'Hilo {i}' for i in range(nt - 1, -1, -1)]
    ax1b.set_yticks(yticks)
    ax1b.set_yticklabels(ylabels, color=T, fontsize=7)
    ax1b.tick_params(colors=T)
    ax1b.grid(axis='x', color=G, alpha=0.4, linestyle='--')
    for s in ['top', 'right']:
        ax1b.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax1b.spines[s].set_color(G)

    # Flecha de speedup entre paneles
    fig.text(0.5, 0.88, f'Speedup: {speedup:.2f}x',
             ha='center', va='center', color=ACC, fontsize=14, fontweight='bold',
             bbox=dict(boxstyle='round,pad=0.4', facecolor='#1a1a2e',
                       edgecolor=ACC, linewidth=2))

    # Leyenda compartida
    legend_elements = [
        mpatches.Patch(color='#3498DB', alpha=0.85, label='Tiempo CPU activo'),
        mpatches.Patch(color='#3498DB', alpha=0.15, label='Wall time (incluye espera)'),
        mpatches.Patch(color=ACC, alpha=0.4, label='Padre esperando (join)'),
    ]
    ax1b.legend(handles=legend_elements, loc='lower right', fontsize=7,
                facecolor=ABG, edgecolor=G, labelcolor=T)

    # ═════════════════════════════════════════════════════════════
    #  FILA 2: ASIGNACIÓN DE CPU POR EL SCHEDULER
    # ═════════════════════════════════════════════════════════════

    # --- 2A: CPU Secuencial ---
    ax2a = fig.add_subplot(gs[1, 0])
    ax2a.set_facecolor(ABG)
    ax2a.set_title('SECUENCIAL — Asignación de CPU por el Scheduler',
                    color='#E74C3C', fontsize=11, fontweight='bold', pad=10)

    if 0 in seq['pc_samples']:
        samples = seq['pc_samples'][0]
        times = [float(s['timestamp_ms']) for s in samples]
        # Dibujar una barra continua en Core 0
        ax2a.barh(0, wall_seq, left=0, height=0.6,
                  color='#E74C3C', alpha=0.8, edgecolor='white', linewidth=0.5)

        # Marcadores de muestreo del PC
        for i, t_ms in enumerate(times):
            if i % (len(times) // 20 + 1) == 0:
                ax2a.plot(t_ms, 0, '|', color='white', markersize=8, alpha=0.6)

    ax2a.set_xlim(-wall_seq * 0.02, wall_seq * 1.05)
    ax2a.set_ylim(-1, 1.5)
    ax2a.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax2a.set_yticks([0])
    ax2a.set_yticklabels(['Core 0'], color=T, fontsize=9)
    ax2a.tick_params(colors=T)
    ax2a.grid(axis='x', color=G, alpha=0.4, linestyle='--')
    for s in ['top', 'right']:
        ax2a.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax2a.spines[s].set_color(G)

    ax2a.text(wall_seq * 0.5, 0.9,
              f'Un solo hilo ocupa 1 core durante {wall_seq:.1f} ms\n'
              f'Los otros {nt - 1} cores permanecen OCIOSOS',
              ha='center', va='center', color='#f85149', fontsize=9,
              fontweight='bold', style='italic',
              bbox=dict(boxstyle='round,pad=0.3', facecolor='#1c1c1c', edgecolor='#f85149', alpha=0.8))

    # Dibujar cores ociosos
    for core in range(1, min(nt, 4)):
        ax2a.barh(-0.2 - core * 0.15, wall_seq, left=0, height=0.1,
                  color='#333', alpha=0.3, edgecolor='#555', linewidth=0.3)

    # --- 2B: CPU Paralelo ---
    ax2b = fig.add_subplot(gs[1, 1])
    ax2b.set_facecolor(ABG)
    ax2b.set_title(f'PARALELO — Asignación de CPU por el Scheduler ({nt} cores)',
                    color='#2ECC71', fontsize=11, fontweight='bold', pad=10)

    for t in par['threads']:
        tid = int(t['thread_id'])
        start = float(t['start_ms'])
        end = float(t['end_ms'])
        c = TC[tid % len(TC)]
        core = tid % nt

        # Barra del hilo en su core
        ax2b.barh(core, end - start, left=start, height=0.65,
                  color=c, alpha=0.8, edgecolor='white', linewidth=0.5)
        ax2b.text(start + (end - start) / 2, core,
                  f'H-{tid}', ha='center', va='center',
                  color='white', fontsize=7, fontweight='bold')

        # Marcar context switches si hay muestras
        if tid in par['pc_samples']:
            samples = par['pc_samples'][tid]
            times = [float(s['timestamp_ms']) for s in samples]
            for j in range(1, len(times)):
                gap = times[j] - times[j - 1]
                avg = (times[-1] - times[0]) / len(times) if len(times) > 1 else gap
                if gap > avg * 4 and avg > 0:
                    mid = (times[j - 1] + times[j]) / 2
                    ax2b.annotate('', xy=(times[j], core + 0.35), xytext=(times[j - 1], core + 0.35),
                                  arrowprops=dict(arrowstyle='<->', color='#f85149', lw=1.5))
                    ax2b.text(mid, core + 0.42, 'CS', ha='center', va='bottom',
                              color='#f85149', fontsize=5.5, fontweight='bold')

    ax2b.set_xlim(-wall_par * 0.02, wall_par * 1.05)
    ax2b.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax2b.set_yticks(range(nt))
    ax2b.set_yticklabels([f'Core {i}' for i in range(nt)], color=T, fontsize=7)
    ax2b.tick_params(colors=T)
    ax2b.grid(axis='x', color=G, alpha=0.4, linestyle='--')
    for s in ['top', 'right']:
        ax2b.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax2b.spines[s].set_color(G)

    ax2b.text(wall_par * 0.5, nt - 0.3,
              f'{nt} hilos distribuidos en {nt} cores por el planificador\n'
              f'CS = Context Switch (cambio de contexto detectado)',
              ha='center', va='center', color='#58a6ff', fontsize=8,
              fontweight='bold', style='italic',
              bbox=dict(boxstyle='round,pad=0.3', facecolor='#1c1c1c', edgecolor='#58a6ff', alpha=0.8))

    # ═════════════════════════════════════════════════════════════
    #  FILA 3: EVOLUCIÓN DEL PCI (Program Counter)
    # ═════════════════════════════════════════════════════════════

    # --- 3A: PCI Secuencial ---
    ax3a = fig.add_subplot(gs[2, 0])
    ax3a.set_facecolor(ABG)
    ax3a.set_title('SECUENCIAL — Evolución del PCI (Program Counter)\n'
                    'Contenido del TCB: Un solo flujo de ejecución lineal',
                    color='#E74C3C', fontsize=10, fontweight='bold', pad=8)

    if 0 in seq['pc_samples']:
        samples = seq['pc_samples'][0]
        times = [float(s['timestamp_ms']) for s in samples]
        pcs_raw = [int(s['pc_addr'], 16) for s in samples]
        pc_base = min(pcs_raw)
        # Mostrar como offset desde la base de rle_compress
        pcs_offset = [(p - pc_base) for p in pcs_raw]

        ax3a.plot(times, pcs_offset, '-', color='#E74C3C', linewidth=1.8, alpha=0.9)
        ax3a.fill_between(times, pcs_offset, alpha=0.1, color='#E74C3C')

        # Marcar inicio y fin
        ax3a.plot(times[0], pcs_offset[0], 'o', color='#2ECC71', markersize=10, zorder=5)
        ax3a.plot(times[-1], pcs_offset[-1], 's', color='#E74C3C', markersize=10, zorder=5)
        ax3a.annotate('INICIO\nPC = rle_compress()', xy=(times[0], pcs_offset[0]),
                       xytext=(times[0] + wall_seq * 0.15, pcs_offset[0] + max(pcs_offset) * 0.3),
                       arrowprops=dict(arrowstyle='->', color='#2ECC71', lw=1.5),
                       color='#2ECC71', fontsize=8, fontweight='bold')
        ax3a.annotate('FIN\nreturn', xy=(times[-1], pcs_offset[-1]),
                       xytext=(times[-1] - wall_seq * 0.15, pcs_offset[-1] - max(pcs_offset) * 0.2),
                       arrowprops=dict(arrowstyle='->', color='#E74C3C', lw=1.5),
                       color='#E74C3C', fontsize=8, fontweight='bold')

        # TCB info box
        ax3a.text(wall_seq * 0.7, max(pcs_offset) * 0.85,
                  f'TCB del Hilo Principal:\n'
                  f'  TID:    {seq["threads"][0]["tid"]}\n'
                  f'  Estado: RUNNING\n'
                  f'  PC:     0x{pc_base:X} → 0x{max(pcs_raw):X}\n'
                  f'  Stack:  {seq["threads"][0]["stack_addr"]}',
                  color=T, fontsize=7.5, fontfamily='monospace',
                  bbox=dict(boxstyle='round,pad=0.4', facecolor='#1c2333',
                            edgecolor='#E74C3C', linewidth=1.5))

    ax3a.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax3a.set_ylabel('PCI — Offset desde base (bytes)', color=T, fontsize=9)
    ax3a.tick_params(colors=T)
    ax3a.grid(color=G, alpha=0.3, linestyle='--')
    for s in ['top', 'right']:
        ax3a.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax3a.spines[s].set_color(G)

    # --- 3B: PCI Paralelo ---
    ax3b = fig.add_subplot(gs[2, 1])
    ax3b.set_facecolor(ABG)
    ax3b.set_title(f'PARALELO — Evolución del PCI ({nt} hilos simultáneos)\n'
                    f'Cada hilo tiene su propio PC en el TCB',
                    color='#2ECC71', fontsize=10, fontweight='bold', pad=8)

    all_pc_base = None
    for tid_k in par['pc_samples']:
        pcs = [int(s['pc_addr'], 16) for s in par['pc_samples'][tid_k]]
        if pcs:
            mn = min(pcs)
            if all_pc_base is None or mn < all_pc_base:
                all_pc_base = mn
    if all_pc_base is None:
        all_pc_base = 0

    for t in par['threads']:
        tid = int(t['thread_id'])
        c = TC[tid % len(TC)]

        if tid in par['pc_samples']:
            samples = par['pc_samples'][tid]
            times = [float(s['timestamp_ms']) for s in samples]
            pcs_raw = [int(s['pc_addr'], 16) for s in samples]
            pcs_offset = [(p - all_pc_base) for p in pcs_raw]

            ax3b.plot(times, pcs_offset, '-', color=c, linewidth=1.3,
                      alpha=0.85, label=f'H-{tid} (TID {t["tid"]})')

            # Marcar inicio
            ax3b.plot(times[0], pcs_offset[0], 'o', color=c, markersize=5, zorder=5)
            ax3b.plot(times[-1], pcs_offset[-1], 's', color=c, markersize=5, zorder=5)

    ax3b.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax3b.set_ylabel('PCI — Offset desde base (bytes)', color=T, fontsize=9)
    ax3b.tick_params(colors=T)
    ax3b.grid(color=G, alpha=0.3, linestyle='--')
    for s in ['top', 'right']:
        ax3b.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax3b.spines[s].set_color(G)
    ax3b.legend(fontsize=6, facecolor=ABG, edgecolor=G,
                labelcolor=T, ncol=4, loc='upper left')

    # Annotation box for parallel
    ax3b.text(wall_par * 0.65, ax3b.get_ylim()[1] * 0.4,
              f'Todos los hilos ejecutan\n'
              f'rle_thread_func() pero cada\n'
              f'TCB tiene su propio PC apuntando\n'
              f'a una posición diferente del código',
              color=T, fontsize=7.5, fontfamily='monospace',
              bbox=dict(boxstyle='round,pad=0.4', facecolor='#1c2333',
                        edgecolor='#2ECC71', linewidth=1.5))

    # ═════════════════════════════════════════════════════════════
    #  FILA 4: PROGRESO Y THROUGHPUT
    # ═════════════════════════════════════════════════════════════

    # --- 4A: Progreso Secuencial ---
    ax4a = fig.add_subplot(gs[3, 0])
    ax4a.set_facecolor(ABG)
    ax4a.set_title('SECUENCIAL — Progreso de Compresión',
                    color='#E74C3C', fontsize=11, fontweight='bold', pad=10)

    if 0 in seq['pc_samples']:
        samples = seq['pc_samples'][0]
        times = [float(s['timestamp_ms']) for s in samples]
        total_px = int(seq['threads'][0]['pixels'])
        pixels = [int(s['pixels_at']) for s in samples]
        pct = [p / total_px * 100 if total_px > 0 else 0 for p in pixels]

        ax4a.fill_between(times, pct, alpha=0.15, color='#E74C3C')
        ax4a.plot(times, pct, '-', color='#E74C3C', linewidth=2, label='Hilo 0 (único)')

        # Línea de 100%
        ax4a.axhline(y=100, color='#2ECC71', linewidth=1, linestyle='--', alpha=0.5)
        ax4a.text(wall_seq * 0.95, 102, '100%', color='#2ECC71', fontsize=8, ha='right')

    ax4a.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax4a.set_ylabel('Progreso (%)', color=T, fontsize=9)
    ax4a.set_ylim(-5, 115)
    ax4a.tick_params(colors=T)
    ax4a.grid(color=G, alpha=0.3, linestyle='--')
    for s in ['top', 'right']:
        ax4a.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax4a.spines[s].set_color(G)
    ax4a.legend(fontsize=8, facecolor=ABG, edgecolor=G, labelcolor=T)

    # Throughput annotation
    total_bytes = int(seq['threads'][0]['pixels']) * 3
    throughput_seq = (total_bytes / 1024 / 1024) / (wall_seq / 1000) if wall_seq > 0 else 0
    ax4a.text(wall_seq * 0.5, 55,
              f'Throughput: {throughput_seq:.0f} MB/s\n'
              f'1 hilo procesa {total_bytes / 1024 / 1024:.1f} MB\n'
              f'en {wall_seq:.1f} ms',
              ha='center', va='center', color=T, fontsize=9,
              bbox=dict(boxstyle='round,pad=0.4', facecolor='#1c2333',
                        edgecolor='#E74C3C', linewidth=1))

    # --- 4B: Progreso Paralelo ---
    ax4b = fig.add_subplot(gs[3, 1])
    ax4b.set_facecolor(ABG)
    ax4b.set_title(f'PARALELO — Progreso de Compresión ({nt} hilos)',
                    color='#2ECC71', fontsize=11, fontweight='bold', pad=10)

    for t in par['threads']:
        tid = int(t['thread_id'])
        c = TC[tid % len(TC)]
        total_px = int(t['pixels'])

        if tid in par['pc_samples']:
            samples = par['pc_samples'][tid]
            times = [float(s['timestamp_ms']) for s in samples]
            pixels = [int(s['pixels_at']) for s in samples]
            pct = [p / total_px * 100 if total_px > 0 else 0 for p in pixels]
            ax4b.plot(times, pct, '-', color=c, linewidth=1.5,
                      alpha=0.85, label=f'H-{tid}')

    ax4b.axhline(y=100, color='#2ECC71', linewidth=1, linestyle='--', alpha=0.5)
    ax4b.text(wall_par * 0.95, 102, '100%', color='#2ECC71', fontsize=8, ha='right')

    ax4b.set_xlabel('Tiempo (ms)', color=T, fontsize=9)
    ax4b.set_ylabel('Progreso (%)', color=T, fontsize=9)
    ax4b.set_ylim(-5, 115)
    ax4b.tick_params(colors=T)
    ax4b.grid(color=G, alpha=0.3, linestyle='--')
    for s in ['top', 'right']:
        ax4b.spines[s].set_visible(False)
    for s in ['bottom', 'left']:
        ax4b.spines[s].set_color(G)
    ax4b.legend(fontsize=6, facecolor=ABG, edgecolor=G,
                labelcolor=T, ncol=4, loc='lower right')

    throughput_par = (total_bytes / 1024 / 1024) / (wall_par / 1000) if wall_par > 0 else 0
    ax4b.text(wall_par * 0.5, 55,
              f'Throughput: {throughput_par:.0f} MB/s\n'
              f'{nt} hilos procesan {total_bytes / 1024 / 1024:.1f} MB\n'
              f'en {wall_par:.1f} ms  ({speedup:.1f}x más rápido)',
              ha='center', va='center', color=T, fontsize=9,
              bbox=dict(boxstyle='round,pad=0.4', facecolor='#1c2333',
                        edgecolor='#2ECC71', linewidth=1))

    # ═════════════════════════════════════════════════════════════
    #  FILA 5: RESUMEN DE CONCEPTOS DE SO
    # ═════════════════════════════════════════════════════════════
    ax5 = fig.add_subplot(gs[4, :])
    ax5.set_facecolor('#0d1927')
    ax5.set_xlim(0, 10)
    ax5.set_ylim(0, 4)
    ax5.axis('off')

    ax5.text(5, 3.7, 'CONCEPTOS DE SISTEMAS OPERATIVOS DEMOSTRADOS',
             ha='center', va='center', color=ACC, fontsize=13, fontweight='bold')

    # Dos columnas de conceptos
    left_x = 0.3
    right_x = 5.3
    concepts = [
        # (emoji-free label, secuencial explanation, paralelo explanation)
        ('HILOS (Threads)',
         f'1 hilo (main) ejecuta todo',
         f'{nt} hilos worker + 1 padre'),
        ('PLANIFICADOR (Scheduler)',
         'Asigna 1 core, los demás ociosos',
         f'Distribuye {nt} hilos en {nt} cores'),
        ('PCI (Program Counter)',
         'Avanza linealmente por rle_compress()',
         f'{nt} PCs avanzan simultáneamente'),
        ('PCB / TCB',
         f'1 PCB: PID={seq["meta"]["pid"]}, 1 TID',
         f'1 PCB + {nt} TCBs con TID, PC, stack propios'),
        ('CAMBIO DE CONTEXTO',
         'Mínimos (1 hilo en 1 core)',
         'El SO conmuta hilos entre cores'),
        ('CONCURRENCIA',
         'No hay: ejecución estrictamente secuencial',
         f'Hilos comparten TEXT/DATA, buffers privados'),
    ]

    for i, (label, sec_val, par_val) in enumerate(concepts):
        y = 3.0 - i * 0.48
        # Label
        ax5.text(left_x, y, f'{label}:', ha='left', va='center',
                 color=ACC, fontsize=8, fontweight='bold')
        # Secuencial
        ax5.text(left_x + 0.05, y - 0.18, f'SEC: {sec_val}',
                 ha='left', va='center', color='#f87171', fontsize=7.5, fontfamily='monospace')
        # Paralelo
        ax5.text(right_x, y, f'{label}:', ha='left', va='center',
                 color=ACC, fontsize=8, fontweight='bold')
        ax5.text(right_x + 0.05, y - 0.18, f'PAR: {par_val}',
                 ha='left', va='center', color='#4ade80', fontsize=7.5, fontfamily='monospace')

    # Metrics box
    ax5.text(5, 0.25,
             f'Wall: {wall_seq:.1f}ms vs {wall_par:.1f}ms  |  '
             f'Speedup: {speedup:.2f}x  |  '
             f'Eficiencia: {efficiency:.1f}%  |  '
             f'CPU total: {cpu_seq:.1f}ms vs {cpu_par:.1f}ms',
             ha='center', va='center', color='white', fontsize=9, fontweight='bold',
             bbox=dict(boxstyle='round,pad=0.4', facecolor='#1a1a2e',
                       edgecolor=ACC, linewidth=2))

    # ═══════════════════════════════════════════════
    plt.savefig(output_path, dpi=150, facecolor=fig.get_facecolor(),
                edgecolor='none', bbox_inches='tight')
    plt.close()
    print(f"\n  Diagrama de Gantt generado: {output_path}")


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(f"Uso: {sys.argv[0]} <csv_secuencial> <csv_paralelo> <output.png>")
        sys.exit(1)
    generate_gantt(sys.argv[1], sys.argv[2], sys.argv[3])
