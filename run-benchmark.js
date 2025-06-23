// Load and run the compression benchmark
load('js/src/shell/compression-benchmark.js');

print("Starting SpiderMonkey Compression Benchmark");
print("Testing both zlib and zstd algorithms with various source sizes\n");

// Run the comprehensive benchmark
runBenchmarks();