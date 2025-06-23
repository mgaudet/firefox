// Test script to demonstrate dual-algorithm compression
// This would be run in a SpiderMonkey shell with TestingFunctions enabled

// Test data - some realistic JavaScript source
const testSource = `
function fibonacci(n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

class Calculator {
    constructor() {
        this.history = [];
    }
    
    add(a, b) {
        const result = a + b;
        this.history.push(\`\${a} + \${b} = \${result}\`);
        return result;
    }
    
    multiply(a, b) {
        const result = a * b;
        this.history.push(\`\${a} * \${b} = \${result}\`);
        return result;
    }
    
    getHistory() {
        return this.history.slice();
    }
}

// More test content to make it larger
const data = Array.from({length: 100}, (_, i) => \`const var\${i} = \${i * Math.PI};\`).join('\\n');
const moreCode = \`
async function processData() {
    try {
        const response = await fetch('/api/data');
        const result = await response.json();
        return result.map(item => ({
            ...item,
            processed: true,
            timestamp: Date.now()
        }));
    } catch (error) {
        console.error('Processing failed:', error);
        throw error;
    }
}
\${data}
\`;
`.repeat(50); // Repeat to make it substantial

print("=== SpiderMonkey Dual-Algorithm Compression Demo ===");
print(`Test source size: ${testSource.length} characters`);

// Test zlib compression (algorithm 0)
print("\n--- Testing zlib compression ---");
try {
    const zlibCompressed = compressString(testSource, 0, 0); // zlib, default level
    print(`zlib compressed size: ${zlibCompressed.byteLength} bytes`);
    
    const zlibDecompressed = decompressString(zlibCompressed);
    const zlibMatches = (zlibDecompressed === testSource);
    print(`zlib decompression matches: ${zlibMatches}`);
    
    if (zlibMatches) {
        const zlibRatio = ((1 - zlibCompressed.byteLength / (testSource.length * 2)) * 100).toFixed(1);
        print(`zlib compression ratio: ${zlibRatio}%`);
    }
} catch (e) {
    print(`zlib test failed: ${e}`);
}

// Test zstd compression (algorithm 1)
print("\n--- Testing zstd compression ---");
try {
    const zstdCompressed = compressString(testSource, 1, 0); // zstd, default level
    print(`zstd compressed size: ${zstdCompressed.byteLength} bytes`);
    
    const zstdDecompressed = decompressString(zstdCompressed);
    const zstdMatches = (zstdDecompressed === testSource);
    print(`zstd decompression matches: ${zstdMatches}`);
    
    if (zstdMatches) {
        const zstdRatio = ((1 - zstdCompressed.byteLength / (testSource.length * 2)) * 100).toFixed(1);
        print(`zstd compression ratio: ${zstdRatio}%`);
    }
} catch (e) {
    print(`zstd test failed: ${e}`);
}

// Test zstd high compression (algorithm 1, level 6)
print("\n--- Testing zstd high compression ---");
try {
    const zstdHighCompressed = compressString(testSource, 1, 6); // zstd, level 6
    print(`zstd-high compressed size: ${zstdHighCompressed.byteLength} bytes`);
    
    const zstdHighDecompressed = decompressString(zstdHighCompressed);
    const zstdHighMatches = (zstdHighDecompressed === testSource);
    print(`zstd-high decompression matches: ${zstdHighMatches}`);
    
    if (zstdHighMatches) {
        const zstdHighRatio = ((1 - zstdHighCompressed.byteLength / (testSource.length * 2)) * 100).toFixed(1);
        print(`zstd-high compression ratio: ${zstdHighRatio}%`);
    }
} catch (e) {
    print(`zstd-high test failed: ${e}`);
}

print("\n=== Compression Demo Complete ===");