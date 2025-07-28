#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/*------------------------------------------------------------------------------------------------------------------
    * Cache Simulator
    * This code simulates a cache system with L1 and L2 caches, handling operations like LOAD, STORE, MODIFY, and INST.
    * It reads a trace file, processes cache accesses, and manages memory blocks.
    * Each of the caches implements write-through policy, meaning that writes go to both L1 and L2 caches.
    * No write-allocate policy is used for STORE operations, meaning that if a STORE misses in L1, it does not fill L1 with the block.
    * For the evictions FİFO policy is used.
    * Please compile with the -lm option to link the math library.
    * Brief explentation of the outputs:
         hits: Number of cache hits in 
         misses: Number of cache misses in
         evictions: Number of evictions in cache
         L1D_final.txt last execution in the L1D cache
         L1I_final.txt last execution in the L1I cache
         L2_final.txt last execution in the L2 cache
    * The code is written in C and uses standard libraries for file handling and string manipulation.
    -------------------------------------------------------------------------------------------------------------------
*/

// Helper prototypes
void read_block_from_ram(FILE *ram_fp, unsigned int address, unsigned char *block, unsigned int block_size);
void write_block_to_ram(FILE *ram_fp, unsigned int address, const unsigned char *data, size_t num_bytes, unsigned int block_size);

void read_trace_file(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
    {
        perror("Failed to open trace file");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        printf("%s", line); // prints each line
    }

    fclose(fp);
}

typedef enum
{
    LOAD,
    STORE,
    MODIFY,
    INST
} OpType;

typedef struct
{
    OpType op;
    unsigned int address;
    int size;
    char data[32]; // Big enough for data bytes as hex string, ram image shows that in the worst case we will have 16 hex characters with extra null it goes up to
                   // 17 we can even shrink it down to data[16] if we want to be more memory efficient but who cares, right ?  :)
} TraceOp;

// tracking everything as just parameters are so much redundant keys pressed - be a good programmer, be compact therefore STRUCT better code :D
typedef struct
{
    int l1_hit;
    int l1_miss;
    int l1_evict;
    int l2_hit;
    int l2_miss;
    int l2_evict;
    int placed_in_l1;
    int placed_in_l2;
    int set_l1, set_l2; // For logging set numbers
    int wrote_to_ram;
} AccessResult;

// didn't need too complicated we have just explicitly extracted all things in the main method
typedef struct
{
    int L1s, L1E, L1b;
    int L2s, L2E, L2b;
    char *tracefile;
} CacheParams;

int parse_trace_line(const char *line, TraceOp *op)
{
    char op_char;
    char addr_str[16] = {0};
    int fields_read = 0;

    // Try to parse with data (S or M)
    if (strchr(line, ','))
    {
        fields_read = sscanf(line, " %c %15[^,], %d, %31s", &op_char, addr_str, &op->size, op->data);
        if (fields_read >= 3)
        {
            // If 4th field not present, set data to empty string
            if (fields_read < 4)
                op->data[0] = '\0';
        }
    }
    else
    {
        // Parse without data (I or L)
        fields_read = sscanf(line, " %c %15s, %d", &op_char, addr_str, &op->size);
        op->data[0] = '\0';
    }

    if (fields_read < 3)
        return 0; // parsing failed

    // Assign operation type
    switch (op_char)
    {
    case 'M':
        op->op = MODIFY;
        break;
    case 'L':
        op->op = LOAD;
        break;
    case 'S':
        op->op = STORE;
        break;
    case 'I':
        op->op = INST;
        break;
    default:
        return 0;
    }
    // Convert hex address string to integer
    op->address = (unsigned int)strtoul(addr_str, NULL, 16);
    return 1;
}

/*int main() {
    read_trace_file("test.trace"); // or any .trace file name
    return 0;
}*/

// cache line structure
typedef struct
{
    int valid;
    unsigned int tag;
    unsigned int fifo_counter; // For FIFO eviction policy
    unsigned char *block;      // Pointer to block data
} CacheLine;

// cache set structure
typedef struct
{
    CacheLine *lines; // Array of lines (E per set)
} CacheSet;

// cache structure
typedef struct
{
    CacheSet *sets;         // Array of sets (S total)
    int s;                  // Number of set index bits
    int E;                  // Associativity (lines per set)
    int b;                  // Number of block offset bits
    unsigned int S;         // Number of sets = 2^s
    unsigned int B;         // Block size = 2^b
    unsigned int fifo_time; // For global FIFO tracking
} Cache;

// allocation logic
Cache *create_cache(int s, int E, int b)
{
    Cache *cache = malloc(sizeof(Cache));
    cache->s = s;
    cache->E = E;
    cache->b = b;
    cache->S = 1 << s;
    cache->B = 1 << b;
    cache->fifo_time = 0;
    cache->sets = malloc(cache->S * sizeof(CacheSet));
    for (unsigned int i = 0; i < cache->S; ++i)
    {
        cache->sets[i].lines = malloc(E * sizeof(CacheLine));
        for (int j = 0; j < E; ++j)
        {
            cache->sets[i].lines[j].valid = 0;
            cache->sets[i].lines[j].tag = 0;
            cache->sets[i].lines[j].fifo_counter = 0;
            cache->sets[i].lines[j].block = malloc(cache->B); // Allocate block size for each line
        }
    }
    return cache;
}

// function to free chaches after the work done : to avoid risky situations and just to be safe
void free_cache(Cache *cache)
{
    if (!cache)
        return;
    for (unsigned int i = 0; i < cache->S; ++i)
    {
        for (int j = 0; j < cache->E; ++j)
        {
            free(cache->sets[i].lines[j].block); // Free each data block
        }
        free(cache->sets[i].lines); // Free lines in each set
    }
    free(cache->sets); // Free sets array
    free(cache);       // Free cache struct itself
}

void print_cache_valid_bits(Cache *cache, const char *name, int sets_to_print)
{
    printf("%s cache valid bits (first %d sets):\n", name, sets_to_print);
    for (int i = 0; i < sets_to_print && i < cache->S; ++i)
    {
        printf("Set %d: ", i);
        for (int j = 0; j < cache->E; ++j)
        {
            printf("%d ", cache->sets[i].lines[j].valid);
        }
        printf("\n");
    }
}

// extract address rather self explanatory
void extract_address_parts_old(unsigned int address, int s, int b,
                               unsigned int *tag, unsigned int *set_index, unsigned int *block_offset)
{
    *block_offset = address & ((1 << b) - 1);
    *set_index = (address >> b) & ((1 << s) - 1);
    *tag = address >> (s + b);
}

// extract address rather self explanatory 3 people worked on this code therefore there are some copyies of the same functions
// left intentionally to show the work done
void extract_address_parts(unsigned address, int s, int b,
                           unsigned *tag, unsigned *set, unsigned *offset)
{
    unsigned mask = (1 << b) - 1;
    *offset = address & mask;
    mask = (1 << s) - 1;
    *set = (address >> b) & mask;
    *tag = address >> (b + s);
}

// Returns index of hit line, or -1 if miss. If miss, also returns index of first invalid or the oldest (FIFO) line to evict.
int find_line(Cache *cache, unsigned int set_index, unsigned int tag, int *victim)
{
    int oldest = 0;
    unsigned int oldest_counter = cache->sets[set_index].lines[0].fifo_counter;

    for (int i = 0; i < cache->E; ++i)
    {
        CacheLine *line = &cache->sets[set_index].lines[i];
        if (line->valid && line->tag == tag)
        {
            return i; // hit!
        }
        if (!line->valid)
        {
            *victim = i; // prefer first invalid
            return -1;
        }
        // go to the oldest for FIFO eviction
        if (line->fifo_counter < oldest_counter)
        {
            oldest_counter = line->fifo_counter;
            oldest = i;
        }
    }
    *victim = oldest;
    return -1; // miss, evict oldest
}

// LOAD in L1D, falling back to L2, and finally to RAM.
void access_load_without_ram(Cache *L1, Cache *L2,
                             unsigned int address,
                             int *L1_hit, int *L1_miss, int *L1_evict,
                             int *L2_hit, int *L2_miss, int *L2_evict, FILE *ram)
{
    // Step 1: L1 lookup
    unsigned int l1_tag, l1_set, l1_off;
    extract_address_parts(address, L1->s, L1->b, &l1_tag, &l1_set, &l1_off);
    int l1_victim;
    int l1_idx = find_line(L1, l1_set, l1_tag, &l1_victim);

    if (l1_idx != -1)
    {
        // HIT
        (*L1_hit)++;
        return;
    }
    // MISS
    (*L1_miss)++;

    // Step 2: L2 lookup
    unsigned int l2_tag, l2_set, l2_off;
    extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
    int l2_victim;
    int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);

    if (l2_idx != -1)
    {
        // L2 HIT
        (*L2_hit)++;
        // (no need to fill L2, but always fill L1 below)
    }
    else
    {
        // L2 MISS
        (*L2_miss)++;
        // Evict if necessary
        if (L2->sets[l2_set].lines[l2_victim].valid)
            (*L2_evict)++;
        // Fill L2 from RAM
        L2->sets[l2_set].lines[l2_victim].valid = 1;
        L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
        L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
    }

    // >>> Always fill L1 after a miss, regardless of L2 hit/miss <<<
    if (L1->sets[l1_set].lines[l1_victim].valid)
        (*L1_evict)++;
    L1->sets[l1_set].lines[l1_victim].valid = 1;
    L1->sets[l1_set].lines[l1_victim].tag = l1_tag;
    L1->sets[l1_set].lines[l1_victim].fifo_counter = L1->fifo_time++;
}

// load operation
void access_load(Cache *L1, Cache *L2,
                 unsigned int address,
                 int *L1_hit, int *L1_miss, int *L1_evict,
                 int *L2_hit, int *L2_miss, int *L2_evict,
                 FILE *ram_fp, AccessResult *result)
{
    // Step 1: L1 lookup
    unsigned int l1_tag, l1_set, l1_off;
    extract_address_parts(address, L1->s, L1->b, &l1_tag, &l1_set, &l1_off);
    int l1_victim;
    int l1_idx = find_line(L1, l1_set, l1_tag, &l1_victim);
    result->set_l1 = l1_set; // we are gonna do this couple of times more to keep everything intact within the result to print the tracking later

    if (l1_idx != -1)
    {
        // HIT
        (*L1_hit)++;
        result->l1_hit = 1;

        return;
    }
    // MISS
    (*L1_miss)++;
    result->l1_miss = 1;

    // Step 2: L2 lookup
    unsigned int l2_tag, l2_set, l2_off;
    extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
    int l2_victim;
    int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);
    result->set_l2 = l2_set;

    if (l2_idx != -1)
    {
        // L2 HIT
        (*L2_hit)++;
        result->l2_hit = 1;
        // (no need to fill L2, but always fill L1 below)
    }
    else
    {
        // L2 MISS
        (*L2_miss)++;
        result->l2_miss = 1;
        // Evict if necessary
        if (L2->sets[l2_set].lines[l2_victim].valid)
        {
            (*L2_evict)++;
            result->l2_evict = 1;
        }
        // Fill L2 from RAM (read block)
        L2->sets[l2_set].lines[l2_victim].valid = 1;
        L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
        L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
        read_block_from_ram(ram_fp, address, L2->sets[l2_set].lines[l2_victim].block, L2->B); // read from ram
        result->placed_in_l2 = 1;
    }

    // >>> Always fill L1 after a miss, regardless of L2 hit/miss <<<
    if (L1->sets[l1_set].lines[l1_victim].valid)
    {
        (*L1_evict)++;
    }
    result->l1_evict = 1;
    L1->sets[l1_set].lines[l1_victim].valid = 1;
    L1->sets[l1_set].lines[l1_victim].tag = l1_tag;
    L1->sets[l1_set].lines[l1_victim].fifo_counter = L1->fifo_time++;
    read_block_from_ram(ram_fp, address, L1->sets[l1_set].lines[l1_victim].block, L1->B); // fill from ram
    result->placed_in_l1 = 1;
}

void access_store_without_ram(Cache *L1, Cache *L2,
                              unsigned int address,
                              int *L1_hit, int *L1_miss, int *L1_evict,
                              int *L2_hit, int *L2_miss, int *L2_evict, AccessResult *res)
{
    // Step 1: L1 lookup
    unsigned int l1_tag, l1_set, l1_off;
    extract_address_parts(address, L1->s, L1->b, &l1_tag, &l1_set, &l1_off);
    int l1_victim;
    int l1_idx = find_line(L1, l1_set, l1_tag, &l1_victim);
    res->set_l1 = l1_set;

    if (l1_idx != -1)
    {
        // HIT
        (*L1_hit)++;
        res->l1_hit = 1;
        // Write-through: also update L2
        unsigned int l2_tag, l2_set, l2_off;
        extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
        int l2_victim;
        int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);
        res->set_l2 = l2_set;

        if (l2_idx != -1)
        {
            (*L2_hit)++;
            res->l2_hit = 1;
        }
        else
        {
            (*L2_miss)++;
            res->l2_miss = 1;
            if (L2->sets[l2_set].lines[l2_victim].valid)
            {
                (*L2_evict)++;
                res->l2_evict = 1;
            }
            L2->sets[l2_set].lines[l2_victim].valid = 1;
            L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
            L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
        }
        return;
    }

    // MISS: No write-allocate
    (*L1_miss)++;
    res->l1_miss = 1;
    // Don't fill L1!
    // Check L2 only
    unsigned int l2_tag, l2_set, l2_off;
    extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
    int l2_victim;
    int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);
    res->set_l2 = l2_set;

    if (l2_idx != -1)
    {
        (*L2_hit)++;
        res->l2_hit = 1;
    }
    else
    {
        (*L2_miss)++;
        res->l2_miss = 1;
        if (L2->sets[l2_set].lines[l2_victim].valid)
        {
            (*L2_evict)++;
            res->l2_evict = 1;
        }
        L2->sets[l2_set].lines[l2_victim].valid = 1;
        L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
        L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
    }
    // Would update RAM here (not needed for stats)
}

// STORE instruction write-through no-write-allocate, block offset aware
void access_store(Cache *L1, Cache *L2,
                  unsigned int address,
                  int *L1_hit, int *L1_miss, int *L1_evict,
                  int *L2_hit, int *L2_miss, int *L2_evict,
                  FILE *ram_fp, TraceOp *op, AccessResult *result)
{
    // Step 1: L1 lookup
    unsigned int l1_tag, l1_set, l1_off;
    extract_address_parts(address, L1->s, L1->b, &l1_tag, &l1_set, &l1_off);
    int l1_victim;
    int l1_idx = find_line(L1, l1_set, l1_tag, &l1_victim);
    result->set_l1 = l1_set; // Store set index for logging, we gonna do that for couple of times to keep everything intact within the result to print the tracking later

    // Prepare data to write (parse from op->data as bytes)
    unsigned char data_bytes[32] = {0};
    size_t num_bytes = strlen(op->data) / 2;
    for (size_t i = 0; i < num_bytes; ++i)
        sscanf(op->data + 2 * i, "%2hhx", &data_bytes[i]);

    unsigned int l1_offset = address & (L1->B - 1); // <-- CHANGE: offset in cache block makes code offset aware

    if (l1_idx != -1)
    {

        (*L1_hit)++;
        result->l1_hit = 1;

        // Update L1 cache line's block at correct offset
        memcpy(L1->sets[l1_set].lines[l1_idx].block + l1_offset, data_bytes, num_bytes);

        // Write-through: also update L2
        unsigned int l2_tag, l2_set, l2_off;
        extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
        int l2_victim;
        int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);
        result->set_l2 = l2_set;

        unsigned int l2_offset = address & (L2->B - 1); // <-- offset in L2 block

        if (l2_idx != -1)
        {
            (*L2_hit)++;
            result->l2_hit = 1;
            memcpy(L2->sets[l2_set].lines[l2_idx].block + l2_offset, data_bytes, num_bytes);
        }
        else
        {
            (*L2_miss)++;
            result->l2_miss = 1;
            if (L2->sets[l2_set].lines[l2_victim].valid)
            {
                (*L2_evict)++;
                result->l2_evict = 1;
            }
            L2->sets[l2_set].lines[l2_victim].valid = 1;
            L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
            L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
            // Fill from RAM first (optional)
            read_block_from_ram(ram_fp, address, L2->sets[l2_set].lines[l2_victim].block, L2->B);
            result->placed_in_l2 = 1;
            // Overwrite bytes at correct offset
            memcpy(L2->sets[l2_set].lines[l2_victim].block + l2_offset, data_bytes, num_bytes);
        }
        // Also write through to RAM at correct offset offset calculated above
        write_block_to_ram(ram_fp, address, data_bytes, num_bytes, L1->B);
        result->wrote_to_ram = 1;
        return;
    }

    // MISS: No write-allocate (don't fill L1!)
    (*L1_miss)++;
    result->l1_miss = 1;

    // Write-through to RAM directly at correct offset
    write_block_to_ram(ram_fp, address, data_bytes, num_bytes, L1->B);
    result->wrote_to_ram = 1;

    // Still check L2 and update
    unsigned int l2_tag, l2_set, l2_off;
    extract_address_parts(address, L2->s, L2->b, &l2_tag, &l2_set, &l2_off);
    int l2_victim;
    int l2_idx = find_line(L2, l2_set, l2_tag, &l2_victim);
    result->set_l2 = l2_set;

    unsigned int l2_offset = address & (L2->B - 1);

    if (l2_idx != -1)
    {
        (*L2_hit)++;
        result->l2_hit = 1;
        memcpy(L2->sets[l2_set].lines[l2_idx].block + l2_offset, data_bytes, num_bytes);
    }
    else
    {
        (*L2_miss)++;
        result->l2_miss = 1;
        if (L2->sets[l2_set].lines[l2_victim].valid)
        {
            (*L2_evict)++;
            result->l2_evict = 1;
        }
        L2->sets[l2_set].lines[l2_victim].valid = 1;
        L2->sets[l2_set].lines[l2_victim].tag = l2_tag;
        L2->sets[l2_set].lines[l2_victim].fifo_counter = L2->fifo_time++;
        read_block_from_ram(ram_fp, address, L2->sets[l2_set].lines[l2_victim].block, L2->B);
        result->placed_in_l2 = 1;
        memcpy(L2->sets[l2_set].lines[l2_victim].block + l2_offset, data_bytes, num_bytes);
    }
    // Would update RAM here (already done above)
}

void access_modify(Cache *L1, Cache *L2,
                   unsigned int address,
                   int *L1_hit, int *L1_miss, int *L1_evict,
                   int *L2_hit, int *L2_miss, int *L2_evict,
                   FILE *ram_fp, TraceOp *op, AccessResult *load_res, AccessResult *store_res) // <-- ADDED ram_fp and op
{
    // First the load part
    memset(load_res, 0, sizeof(AccessResult));
    access_load(L1, L2, address, L1_hit, L1_miss, L1_evict,
                L2_hit, L2_miss, L2_evict, ram_fp, load_res);
    // Then the store part
    memset(store_res, 0, sizeof(AccessResult));
    access_store(L1, L2, address, L1_hit, L1_miss, L1_evict,
                 L2_hit, L2_miss, L2_evict, ram_fp, op, store_res);
}

/*-------------------------- OLDER MAIN USED FOR TESTING --------------------------
int main(int argc, char *argv[])
{
    int L1s = -1, L1E = -1, L1b = -1, L2s = -1, L2E = -1, L2b = -1;
    char tracefile[256] = {0};

    // Parse command line arguments
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-L1s"))
            L1s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L1E"))
            L1E = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L1b"))
            L1b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2s"))
            L2s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2E"))
            L2E = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2b"))
            L2b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t"))
            strncpy(tracefile, argv[++i], 255);
        else
        {
            printf("Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (L1s < 0 || L1E < 0 || L1b < 0 || L2s < 0 || L2E < 0 || L2b < 0 || tracefile[0] == 0)
    {
        printf("Usage: %s -L1s <L1s> -L1E <L1E> -L1b <L1b> -L2s <L2s> -L2E <L2E> -L2b <L2b> -t <tracefile>\n", argv[0]);
        return 1;
    }

    // Create caches and check
    Cache *L1D = create_cache(L1s, L1E, L1b);
    Cache *L1I = create_cache(L1s, L1E, L1b);
    Cache *L2 = create_cache(L2s, L2E, L2b);

    if (!L1D || !L1I || !L2)
    {
        printf("Failed to allocate one or more caches!\n");
        free_cache(L1D);
        free_cache(L1I);
        free_cache(L2);
        return 1;
    }

    printf("L1D: %u sets, %d lines/set, %u bytes/block\n", L1D->S, L1D->E, L1D->B);
    printf("L1I: %u sets, %d lines/set, %u bytes/block\n", L1I->S, L1I->E, L1I->B);
    printf("L2: %u sets, %d lines/set, %u bytes/block\n", L2->S, L2->E, L2->B);

    print_cache_valid_bits(L1D, "L1D", 4);
    print_cache_valid_bits(L1I, "L1I", 4);
    print_cache_valid_bits(L2, "L2", 4);

    // ---- Begin demo for address extraction ----
    FILE *fp = fopen(tracefile, "r");
    if (!fp)
    {
        perror("Failed to open trace file");
        free_cache(L1D);
        free_cache(L1I);
        free_cache(L2);
        return 1;
    }

    char line[256];
    TraceOp op;
    unsigned int tag, set_index, block_offset;
    int op_count = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (parse_trace_line(line, &op))
        {
            printf("\nOperation %d:\n", ++op_count);
            printf("  Trace: %s", line);

            // For L1D
            extract_address_parts(op.address, L1D->s, L1D->b, &tag, &set_index, &block_offset);
            printf("  L1D: tag=0x%x set=0x%x offset=0x%x\n", tag, set_index, block_offset);

            // For L2
            extract_address_parts(op.address, L2->s, L2->b, &tag, &set_index, &block_offset);
            printf("  L2:  tag=0x%x set=0x%x offset=0x%x\n", tag, set_index, block_offset);
        }
        else
        {
            printf("Failed to parse: %s", line);
        }
    }
    fclose(fp);

    free_cache(L1D);
    free_cache(L1I);
    free_cache(L2);
    printf("All caches freed!\n");
    return 0;
} */

// this is not used because of structure disparity
Cache *create_cache_old(int s, int E, int b)
{
    Cache *cache = (Cache *)malloc(sizeof(Cache));
    cache->s = s;
    cache->E = E;
    cache->b = b;
    cache->S = (unsigned)pow(2, s);
    cache->B = (unsigned)pow(2, b);
    //    cache->fifo_next = 0;
    //   cache->hits = 0;
    //  cache->misses = 0;
    //    cache->evictions = 0;

    cache->sets = (CacheSet *)malloc(cache->S * sizeof(CacheSet));
    for (unsigned i = 0; i < cache->S; i++)
    {
        cache->sets[i].lines = (CacheLine *)malloc(E * sizeof(CacheLine));
        for (int j = 0; j < E; j++)
        {
            cache->sets[i].lines[j].valid = 0;
            cache->sets[i].lines[j].tag = 0;
            //        cache->sets[i].lines[j].fifo_seq = 0;
            cache->sets[i].lines[j].block = (unsigned char *)malloc(cache->B);
        }
    }
    return cache;
}

void print_cache_to_file(Cache *cache, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp)
    {
        perror("Failed to open output file");
        return;
    }
    for (unsigned int i = 0; i < cache->S; ++i)
    {
        fprintf(fp, "Set %u:\n", i);
        for (int j = 0; j < cache->E; ++j)
        {
            CacheLine *line = &cache->sets[i].lines[j];
            if (line->valid)
            {
                fprintf(fp, "  Line %d: Valid=1, Tag=0x%x, Time=%u, Data=", j, line->tag, line->fifo_counter);
                // Print block data as hex string
                for (unsigned int k = 0; k < cache->B; ++k)
                    fprintf(fp, "%02x", line->block[k]);
                fprintf(fp, "\n");
            }
            else
            {
                fprintf(fp, "  Line %d: Valid=0, Tag=-\n", j);
            }
        }
    }
    fclose(fp);
}

// helper function for RAM operations
void read_block_from_ram(FILE *ram_fp, unsigned int address, unsigned char *block, unsigned int block_size)
{
    fseek(ram_fp, address & ~(block_size - 1), SEEK_SET);
    fread(block, 1, block_size, ram_fp);
}

// overwrites the whole block to the ram may overwrite irrelavent data
void write_block_to_ram_old(FILE *ram_fp, unsigned int address, const unsigned char *block, unsigned int block_size)
{
    fseek(ram_fp, address & ~(block_size - 1), SEEK_SET);
    fwrite(block, 1, block_size, ram_fp);
    fflush(ram_fp);
}

// corrected version of write_block_to_ram it will write only the relevant part of the data
void write_block_to_ram(FILE *ram_fp, unsigned int address, const unsigned char *data, size_t num_bytes, unsigned int block_size)
{
    unsigned char temp_block[64] = {0}; // <-- maximum block size if tended to use a bigger block change this
    unsigned int block_start = address & ~(block_size - 1);
    unsigned int offset = address - block_start;
    fseek(ram_fp, block_start, SEEK_SET);
    fread(temp_block, 1, block_size, ram_fp);     // Read full block
    memcpy(temp_block + offset, data, num_bytes); // Only overwrite what you need
    fseek(ram_fp, block_start, SEEK_SET);
    fwrite(temp_block, 1, block_size, ram_fp);
    fflush(ram_fp);
}

// Call this before or after each op as fits your logic
void log_operation(const TraceOp *op,
                   const char *l1_result, const char *l2_result,
                   const char *placement_or_action)
{
    // Print operation type
    switch (op->op)
    {
    case LOAD:
        printf("L %x, %d\n", op->address, op->size);
        break;
    case STORE:
        printf("S %x, %d, %s\n", op->address, op->size, op->data);
        break;
    case MODIFY:
        printf("M %x, %d, %s\n", op->address, op->size, op->data);
        break;
    case INST:
        printf("I %x, %d\n", op->address, op->size);
        break;
    }
    // Print L1 and L2 result lines, if not NULL
    if (l1_result && *l1_result)
        printf("  %s\n", l1_result);
    if (l2_result && *l2_result)
        printf("  %s\n", l2_result);
    // Placement or action info, if any
    if (placement_or_action && *placement_or_action)
        printf("  %s\n", placement_or_action);
}

int main(int argc, char *argv[])
{
    int L1s = -1, L1E = -1, L1b = -1, L2s = -1, L2E = -1, L2b = -1;
    char tracefile[256] = {0};

    // Parse command line arguments (unchanged)
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-L1s"))
            L1s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L1E"))
            L1E = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L1b"))
            L1b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2s"))
            L2s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2E"))
            L2E = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-L2b"))
            L2b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t"))
            strncpy(tracefile, argv[++i], 255);
        else
        {
            printf("Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (L1s < 0 || L1E < 0 || L1b < 0 || L2s < 0 || L2E < 0 || L2b < 0 || tracefile[0] == 0)
    {
        printf("Usage: %s -L1s <L1s> -L1E <L1E> -L1b <L1b> -L2s <L2s> -L2E <L2E> -L2b <L2b> -t <tracefile>\n", argv[0]);
        return 1;
    }

    // Create caches
    Cache *L1D = create_cache(L1s, L1E, L1b);
    Cache *L1I = create_cache(L1s, L1E, L1b);
    Cache *L2 = create_cache(L2s, L2E, L2b);

    if (!L1D || !L1I || !L2)
    {
        printf("Failed to allocate one or more caches!\n");
        free_cache(L1D);
        free_cache(L1I);
        free_cache(L2);
        return 1;
    }

    // RAM.dat must be large enough to cover the highest address accessed by any trace. Files given before therefore no checks made regarding this situation
    // read RAM data
    FILE *ram_fp = fopen("RAM.dat", "rb+"); // use rb+ for both reading and writing
    if (!ram_fp)
    {
        perror("Failed to open RAM.dat");
        free_cache(L1D);
        free_cache(L1I);
        free_cache(L2);
        return 1;
    }

    // Stats counters
    int L1D_hit = 0, L1D_miss = 0, L1D_evict = 0;
    int L1I_hit = 0, L1I_miss = 0, L1I_evict = 0;
    int L2_hit = 0, L2_miss = 0, L2_evict = 0;

    // Open trace file
    FILE *fp = fopen(tracefile, "r");
    if (!fp)
    {
        perror("Failed to open trace file");
        free_cache(L1D);
        free_cache(L1I);
        free_cache(L2);
        return 1;
    }

    char line[256];
    TraceOp op;

    // to track the operations on instructions, use with the helper function track_operation
    char op_line[128] = {0};
    char l1_res[64] = {0};
    char l2_res[64] = {0};  // we have learned optimization therefore, these are not in the loop anymore :)
    char action[128] = {0};
    AccessResult load_res = {0};
    AccessResult store_res = {0};

    while (fgets(line, sizeof(line), fp))
    {
        if (parse_trace_line(line, &op))
        {
            // Clear string buffers
            op_line[0] = l1_res[0] = l2_res[0] = action[0] = '\0';
            // Clear struct contents
            memset(&load_res, 0, sizeof(AccessResult));
            memset(&store_res, 0, sizeof(AccessResult));

            if (op.op == LOAD)
            {
                access_load(L1D, L2, op.address, &L1D_hit, &L1D_miss, &L1D_evict,
                                        &L2_hit, &L2_miss, &L2_evict, ram_fp, &load_res);

                sprintf(op_line, "\nL %x, %d", op.address, op.size);
                if (load_res.l1_hit)
                    strcpy(l1_res, "L1D hit");
                else if (load_res.l1_miss)
                    strcpy(l1_res, "L1D miss");
                if (load_res.l2_hit)
                    strcpy(l2_res, "L2 hit");
                else if (load_res.l2_miss)
                    strcpy(l2_res, "L2 miss");
                if (load_res.placed_in_l1)
                    strcpy(action, "Place in L1D");
                else if (load_res.placed_in_l2)
                    strcpy(action, "Place in L2");
            }
            else if (op.op == INST)
            {
                access_load(L1I, L2, op.address, &L1I_hit, &L1I_miss, &L1I_evict,
                                        &L2_hit, &L2_miss, &L2_evict, ram_fp, &load_res);

                sprintf(op_line, "\nI %x, %d", op.address, op.size);
                if (load_res.l1_hit)
                    strcpy(l1_res, "L1I hit");
                else if (load_res.l1_miss)
                    strcpy(l1_res, "L1I miss");
                if (load_res.l2_hit)
                    strcpy(l2_res, "L2 hit");
                else if (load_res.l2_miss)
                    strcpy(l2_res, "L2 miss");
                if (load_res.placed_in_l1)
                    strcpy(action, "Place in L1I");
                else if (load_res.placed_in_l2)
                    strcpy(action, "Place in L2");
            }
            else if (op.op == STORE)
            {
                access_store(L1D, L2, op.address, &L1D_hit, &L1D_miss, &L1D_evict,
                                         &L2_hit, &L2_miss, &L2_evict, ram_fp, &op, &store_res);

                sprintf(op_line, "\nS %x, %d, %s", op.address, op.size, op.data);
                if (store_res.l1_hit)
                    strcpy(l1_res, "L1D hit");
                else if (store_res.l1_miss)
                    strcpy(l1_res, "L1D miss");
                if (store_res.l2_hit)
                    strcpy(l2_res, "L2 hit");
                else if (store_res.l2_miss)
                    strcpy(l2_res, "L2 miss");
                if (store_res.l1_hit && store_res.l2_hit && store_res.wrote_to_ram)
                    strcpy(action, "Store in L1D, L2, RAM");
                else if (store_res.l2_hit && store_res.wrote_to_ram)
                    strcpy(action, "Store in L2, RAM");
                else if (store_res.wrote_to_ram)
                    strcpy(action, "Store in RAM");
            }
            else if (op.op == MODIFY)
            {
                access_modify(L1D, L2, op.address, &L1D_hit, &L1D_miss, &L1D_evict,
                                          &L2_hit, &L2_miss, &L2_evict, ram_fp, &op, &load_res, &store_res);

                sprintf(op_line, "\nM %x, %d, %s", op.address, op.size, op.data);

                // Summarize the LOAD result
                if (load_res.l1_hit)
                    strcpy(l1_res, "L1D hit");
                else if (load_res.l1_miss)
                    strcpy(l1_res, "L1D miss");
                if (load_res.l2_hit)
                    strcpy(l2_res, "L2 hit");
                else if (load_res.l2_miss)
                    strcpy(l2_res, "L2 miss");

                // Summarize the STORE result
                if (store_res.l1_hit && store_res.l2_hit && store_res.wrote_to_ram)
                    strcpy(action, "Store in L1D, L2, RAM");
                else if (store_res.l2_hit && store_res.wrote_to_ram)
                    strcpy(action, "Store in L2, RAM");
                else if (store_res.wrote_to_ram)
                    strcpy(action, "Store in RAM");
            }

            // === Print the log in requested format ===
            printf("%s\n", op_line);
            if (strlen(l1_res))
            {
                printf("  %s", l1_res);
                if (strlen(l2_res))
                    printf(", %s\n", l2_res);
                else
                    printf("\n");
            }
            else if (strlen(l2_res))
            {
                printf("  %s\n", l2_res);
            }
            if (strlen(action))
                printf("  %s\n", action);
        }
    }
    fclose(fp);

    // Print stats in required format
    printf("\n");
    printf("L1I-hits:%d L1I-misses:%d L1I-evictions:%d\n", L1I_hit, L1I_miss, L1I_evict);
    printf("L1D-hits:%d L1D-misses:%d L1D-evictions:%d\n", L1D_hit, L1D_miss, L1D_evict);
    printf("L2-hits:%d L2-misses:%d L2-evictions:%d\n", L2_hit, L2_miss, L2_evict);

    // printf("\nReached a milestone! \n\n"); <-- debug

    print_cache_to_file(L1D, "L1D_final.txt");
    print_cache_to_file(L1I, "L1I_final.txt");
    print_cache_to_file(L2, "L2_final.txt");

    free_cache(L1D);
    free_cache(L1I);
    free_cache(L2);
    //printf("All caches freed!\n"); debug 
    return 0;
}

/*
===================== TEST RESULTS =====================
./main -L1s 1 -L1E 2 -L1b 4 -L2s 2 -L2E 2 -L2b 4 -t test_small.trace
L1I-hits:0 L1I-misses:2 L1I-evictions:0
L1D-hits:3 L1D-misses:3 L1D-evictions:0
L2-hits:3 L2-misses:5 L2-evictions:1

./main -L1s 2 -L1E 2 -L1b 5 -L2s 3 -L2E 4 -L2b 5 -t test_medium.trace
L1I-hits:0 L1I-misses:41 L1I-evictions:33
L1D-hits:20 L1D-misses:59 L1D-evictions:37
L2-hits:20 L2-misses:100 L2-evictions:68

./main -L1s 4 -L1E 4 -L1b 6 -L2s 6 -L2E 8 -L2b 6 -t test_large.trace
L1I-hits:256 L1I-misses:3687 L1I-evictions:3623
L1D-hits:2056 L1D-misses:6038 L1D-evictions:3967
L2-hits:2694 L2-misses:9071 L2-evictions:8559

*/

/*
    Changes:
    1. The original structs did not have miss, hit, and eviction counters. So they are added to the main function. For the same reason i did not used the print stats
    2. The original code did not have a way to read/write from/to RAM, so I added the functions read_block_from_ram and write_block_to_ram.
    3. Used the same loop for access_load and access_store, so I added a TraceOp struct to pass the data for the store operation.
    4. The original code did not have a way to print the cache contents to a file, so I added the print_cache_to_file function.
    5. Some of the functions have changed due to disparity between structures.

*/