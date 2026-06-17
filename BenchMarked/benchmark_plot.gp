set terminal pngcairo size 1000,600 enhanced font 'Arial,12'
set output 'benchmark_plot.png'
set title 'Custom Allocator Benchmark Comparison'
set style data histogram
set style histogram clustered gap 1
set style fill solid border -1
set boxwidth 0.7
set grid ytics
set ylabel 'Time (µs)'
set xtics format ''
set key outside top center horizontal
plot 'benchmark_data.dat' using 2:xtic(1) title 'PoolAllocator', \
     '' using 3 title 'Standard new/delete'
