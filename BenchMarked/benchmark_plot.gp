set terminal pngcairo size 1100,650 enhanced font 'Arial,13'
set output 'benchmark_plot.png'

# ── Colours ────────────────────────────────────────────────────────────────
pool_color  = "#7B2FBE"   # purple  — pool allocator
heap_color  = "#2E8B57"   # green   — standard new/delete

# ── Layout ────────────────────────────────────────────────────────────────
set title 'Data-Split Pool Allocator vs Standard Heap\n{/*0.85 1,000,000 orders | -O3 -march=native | 7-run median | steady\_clock}' \
    font 'Arial Bold,14'

set style data histogram
set style histogram clustered gap 1.2
set style fill solid 0.88 border -1
set boxwidth 0.75

set grid ytics lt 0 lc rgb "#cccccc" lw 0.5
set border 3 lw 1.2

set ylabel 'Time (µs)' font 'Arial,12' offset -1,0
set yrange [0:*]
set ytics font 'Arial,11'

# X-axis labels come from column 1 of the .dat file
set xtics font 'Arial Bold,12' scale 0
set xtics ("Allocation\n(1M orders)" 0, \
           "Hot Scan\n(price field)" 1, \
           "Cold Scan\n(order_id)" 2)

set key outside top center horizontal font 'Arial,11' samplen 1.5 spacing 1.2

# ── Ratio annotations ─────────────────────────────────────────────────────
# Placed above the taller (heap) bar for each group
# Group centres in histogram clustered mode: 0, 1, 2
# Each bar is 0.75 wide, gap 1.2 — heap bar is at +0.42 from group centre

set label 1 "3.13×\nfaster" at 0.42, 47500  center font 'Arial Bold,11' tc rgb "#1a1a1a"
set label 2 "2.42×\nfaster" at 1.42, 4200   center font 'Arial Bold,11' tc rgb "#1a1a1a"
set label 3 "by design\n(out-of-band)" at 2.42, 4200 center font 'Arial,10' tc rgb "#666666"

# ── Plot ──────────────────────────────────────────────────────────────────
plot 'benchmark_data.dat' \
        using 2:xtic(1) title 'Pool Allocator (SoA)'  lc rgb pool_color, \
     ''  using 3         title 'Standard new/delete'   lc rgb heap_color
