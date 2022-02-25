# CS3650-Allocator
Memory allocator based on CS3650 challenge assignment

## Description

This is somewhat more complicated memory allocator designed to optimize for the included vector and list based collatz programs to improve performance. This allocator `opt_malloc.c` utilizes per-CPU arenas, block allocation with shared metadata, and per-block locking to improve performance past that of a simplistic free-list only allocator. According to the results shown in `report.txt` and `graph.png`, the optimized allocator could beat the system `malloc` on the Debian 10 VirtualBox VM.

## How to run

To run the test suite provided by the class, run the following commands

```
$ make
$ make test
```

This will run some rudimentary testing to make sure the results obtained correspond to the expect results of the program for three different allocators
1. The system allocator
2. The simple free-list allocator
3. The optimized allocator
