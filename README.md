# Cache Simulator

A C program that simulates a two-level (L1 and L2) cache hierarchy with configurable parameters. Supports FIFO eviction, write-through, and no write-allocate policies.  
The simulator processes memory access traces and reports cache hits, misses, and evictions.

## Features

- Simulates L1 (separate data and instruction caches) and L2 caches
- FIFO (First-In-First-Out) eviction policy
- Write-through, no write-allocate on STORE
- Supports LOAD, STORE, MODIFY, and INST operations
- Reads configurable memory traces
- Outputs cache statistics and final cache contents

## Compilation

Make sure you have a C compiler (like gcc).  
The math library (`-lm`) is required for compilation.

```bash
gcc -o cache_sim main.c -lm
