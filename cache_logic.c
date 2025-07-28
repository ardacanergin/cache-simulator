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

// LOAD in L1D, falling back to L2, and finally to RAM.
void access_load(Cache *L1, Cache *L2,
                 unsigned int address,
                 int *L1_hit, int *L1_miss, int *L1_evict,
                 int *L2_hit, int *L2_miss, int *L2_evict)
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
        // (optionally: load block data)
    }

    // >>> Always fill L1 after a miss, regardless of L2 hit/miss <<<
    if (L1->sets[l1_set].lines[l1_victim].valid)
        (*L1_evict)++;
    L1->sets[l1_set].lines[l1_victim].valid = 1;
    L1->sets[l1_set].lines[l1_victim].tag = l1_tag;
    L1->sets[l1_set].lines[l1_victim].fifo_counter = L1->fifo_time++;
    // (optionally: copy block data from L2 or RAM)
}