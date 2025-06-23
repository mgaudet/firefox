// Compression benchmark for SpiderMonkey
// Tests compression and decompression speed with various source sizes

function generateSource(size) {
    // Generate realistic JavaScript source code of approximately the given size
    let source = "";
    const templates = [
        "function func_NAME() { return VALUE; }",
        "const var_NAME = VALUE;",
        "if (VALUE > VALUE) { console.log('test'); }",
        "for (let i = 0; i < VALUE; i++) { total += i; }",
        "class Class_NAME { constructor() { this.value = VALUE; } }",
        "// This is a comment line that adds some bulk to the source\n",
        "/* Multi-line comment\n * that spans multiple lines\n * to add realistic content */\n",
        "const obj_NAME = { key1: VALUE, key2: 'string', key3: [1,2,3] };",
        "try { doSomething(); } catch(e) { console.error(e); }",
        "async function async_NAME() { await Promise.resolve(VALUE); }"
    ];
    
    let counter = 0;
    while (source.length < size) {
        const template = templates[counter % templates.length];
        const line = template
            .replace(/NAME/g, counter)
            .replace(/VALUE/g, Math.floor(Math.random() * 1000));
        source += line + "\n";
        counter++;
    }
    
    return source.substring(0, size);
}

function formatBytes(bytes) {
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + " KB";
    return (bytes / (1024 * 1024)).toFixed(2) + " MB";
}

function formatTime(ms) {
    if (ms < 1) return (ms * 1000).toFixed(2) + " µs";
    if (ms < 1000) return ms.toFixed(2) + " ms";
    return (ms / 1000).toFixed(2) + " s";
}

function benchmarkAlgorithm(algorithmName, algorithm, level, source) {
    const iterations = 10;
    let compressTimes = [];
    let decompressTimes = [];
    let compressedSizes = [];
    
    // Warm up
    let compressed = compressString(source, algorithm, level);
    decompressString(compressed);
    
    // Benchmark compression
    for (let i = 0; i < iterations; i++) {
        const start = dateNow();
        compressed = compressString(source, algorithm, level);
        const end = dateNow();
        compressTimes.push(end - start);
        compressedSizes.push(compressed.byteLength);
    }
    
    // Benchmark decompression
    for (let i = 0; i < iterations; i++) {
        const start = dateNow();
        const decompressed = decompressString(compressed);
        const end = dateNow();
        decompressTimes.push(end - start);
        
        // Verify correctness
        if (decompressed !== source) {
            throw new Error(`Decompressed string doesn't match original (${algorithmName})!`);
        }
    }
    
    const avgCompressTime = compressTimes.reduce((a, b) => a + b) / iterations;
    const avgDecompressTime = decompressTimes.reduce((a, b) => a + b) / iterations;
    const avgCompressedSize = compressedSizes.reduce((a, b) => a + b) / iterations;
    const compressionRatio = ((source.length * 2 - avgCompressedSize) / (source.length * 2)) * 100;
    const compressSpeed = (source.length * 2) / (avgCompressTime / 1000) / (1024 * 1024); // MB/s
    const decompressSpeed = (source.length * 2) / (avgDecompressTime / 1000) / (1024 * 1024); // MB/s
    
    return {
        algorithm: algorithmName,
        avgCompressTime,
        avgDecompressTime,
        avgCompressedSize,
        compressionRatio,
        compressSpeed,
        decompressSpeed
    };
}

function benchmark(name, source) {
    print(`\n=== ${name} ===`);
    print(`Source size: ${formatBytes(source.length * 2)} (${source.length} characters)`);
    
    // Test zlib (algorithm 0) with default level
    const zlibResults = benchmarkAlgorithm("zlib", 0, 0, source);
    
    // Test zstd (algorithm 1) with default level  
    const zstdResults = benchmarkAlgorithm("zstd", 1, 0, source);
    
    // Test zstd with higher compression level
    const zstdHighResults = benchmarkAlgorithm("zstd-high", 1, 6, source);
    
    // Print results
    print(`\nResults:`);
    print(`Algorithm     | Compress Time | Decompress Time | Compressed Size | Ratio | Comp Speed | Decomp Speed`);
    print(`------------- | ------------- | --------------- | --------------- | ----- | ---------- | ------------`);
    
    [zlibResults, zstdResults, zstdHighResults].forEach(r => {
        print(`${r.algorithm.padEnd(13)} | ${formatTime(r.avgCompressTime).padEnd(13)} | ${formatTime(r.avgDecompressTime).padEnd(15)} | ${formatBytes(r.avgCompressedSize).padEnd(15)} | ${r.compressionRatio.toFixed(1).padEnd(5)}% | ${r.compressSpeed.toFixed(1).padEnd(10)} MB/s | ${r.decompressSpeed.toFixed(1).padEnd(12)} MB/s`);
    });
    
    // Print comparison
    const zlibTime = zlibResults.avgCompressTime;
    const zstdTime = zstdResults.avgCompressTime;
    const zstdSpeedup = zlibTime / zstdTime;
    const sizeImprovement = ((zlibResults.avgCompressedSize - zstdResults.avgCompressedSize) / zlibResults.avgCompressedSize) * 100;
    
    print(`\nComparison:`);
    print(`- zstd is ${zstdSpeedup.toFixed(2)}x ${zstdSpeedup > 1 ? 'faster' : 'slower'} than zlib for compression`);
    print(`- zstd achieves ${sizeImprovement.toFixed(1)}% ${sizeImprovement > 0 ? 'better' : 'worse'} compression ratio than zlib`);
    
    // Check if we're using chunking  
    const numChunks = Math.ceil((source.length * 2) / (64 * 1024));
    if (numChunks > 1) {
        print(`Number of chunks: ${numChunks}`);
    }
}

function runBenchmarks() {
    print("SpiderMonkey Compression Benchmark");
    print("==================================");
    
    // Small sources (< 1 chunk)
    benchmark("Small source (1 KB)", generateSource(512));
    benchmark("Small source (10 KB)", generateSource(5 * 1024));
    benchmark("Small source (32 KB)", generateSource(16 * 1024));
    
    // Medium sources (1-5 chunks)
    benchmark("Medium source (64 KB - 1 chunk boundary)", generateSource(32 * 1024));
    benchmark("Medium source (128 KB - 2 chunks)", generateSource(64 * 1024));
    benchmark("Medium source (256 KB - 4 chunks)", generateSource(128 * 1024));
    
    // Large sources (many chunks)
    benchmark("Large source (1 MB)", generateSource(512 * 1024));
    benchmark("Large source (5 MB)", generateSource(2.5 * 1024 * 1024));
    benchmark("Large source (10 MB)", generateSource(5 * 1024 * 1024));
    
    // Edge cases for chunking
    print("\n=== Chunk Boundary Tests ===");
    
    // Exactly one chunk (64KB = 32K chars)
    const oneChunk = generateSource(32 * 1024);
    benchmark("Exactly 1 chunk (64 KB)", oneChunk);
    
    // Just over one chunk
    const justOverOneChunk = generateSource(32 * 1024 + 100);
    benchmark("Just over 1 chunk (64 KB + 200 bytes)", justOverOneChunk);
    
    // Multiple exact chunks
    const exactChunks = generateSource(32 * 1024 * 3);
    benchmark("Exactly 3 chunks (192 KB)", exactChunks);
}

// Run the benchmarks
try {
    runBenchmarks();
} catch (e) {
    print("Error running benchmarks: " + e);
    print(e.stack);
}