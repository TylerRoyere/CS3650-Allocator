
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <string.h> 
#include <stdint.h>
#include <assert.h>
#include "xmalloc.h"

#define NUM_ARENAS 16  // number of arenas used for allocation
#define NUM_BUCKETS 26 // number of buckets to use with the current

// this is the data provided for larger allocations that use
// a different strucutre (raw mmap)
typedef struct header {
    size_t size; 
    long arena;
} header;

//TODO:
// - how to decide the number of entries to keep track of using
//   used field for a given size
// - How to easily map used field to address of block in the struct
//
// Strategy:
//  - allocate xxxKB blocks for all bucket sizes,
//  - store all of the information in the first page
//    where every entry gets a bit in a char array
//    for its used field
//  - Whenever anything is freed set the corresponding bit
//    to that entry to 0;
//  - All blocks will be aligned to this size, meaning that
//    the meta data for the block can always be found for a given
//    entry by just masking off the bottom bits
//
// xxKB aligned ---> +--------------+
//                   | size, arena  | 
//                   | first, lock  |
//                   +--------------+ 
//                   | Used chars   |
//                   +--------------+
//                   |   data       |
//                   |  ........    |
//                   |              |
//                   +--------------+

// information block allocation 
typedef struct block_md{
    unsigned int size;    // size of the entries in the block
    unsigned int arena;   // arena the block is located in
    unsigned int first_off;    // lowest id (bit index) avail. for alloc
    unsigned int references; // total used entries in block
    pthread_mutex_t lock; // lock for the data in the block
    struct block_md* next; // pointer to the next entry in the list 
    struct block_md* prev; // pointer to the previous entry in the list
} block_md;

static const int PAGE_SIZE = 4096; // standard number of bytes per page
                            // alignment strategy, chosen heuristically
                            
static const int ALIGNMENT = (128 * 1024); // alignment constant
static const int ivec_init_size = 32;

// array of sizes available in buckets
static long block_sizes[NUM_BUCKETS] = {1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 
                                       128, 192, 256, 384, 512, 768, 1024,
                                       1536, 2048, 3072, 4096, 6144, 8192,
                                       12292, 16384, 24580/*, 32768, 49160,
                                       65536, 114696*/};

// Arenas will hold the pointers to blocks with spaces left,
// this way blocks with space are still available for future uses
static block_md* arenas[NUM_ARENAS][NUM_BUCKETS];

// if another thread free'd all of the entries in a bucket
// they can flag the "owning" thread that thes buckets are available
// for reuse without having to unmap them
static int reusable_flag[NUM_ARENAS];
static int reusable[NUM_ARENAS][NUM_BUCKETS];

// arena locks one for the list, one for allocating one for freeing
static pthread_mutex_t locks[NUM_ARENAS][NUM_BUCKETS];

// structure for deciding thread arenas
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static int arena_taken[NUM_ARENAS];

// Thread local variable for whatever arena this thread is using
static long __thread thread_arena = -1;

// determines the index of the smallest block size that is large
// enough to hold the specified number of bytes
static
inline
long
get_size_index(size_t bytes)
{
    // optimizing for the common cases
    if(bytes == 16) {
        return 4;
    }
    else if(bytes == 24) {
        return 5;
    }
    else if(bytes == 32) {
        return 6;
    } 

    for(int ii = 0; ii < NUM_BUCKETS; ii+=4) {
        if(block_sizes[ii] >= bytes) {
            return ii;
        }
        if(block_sizes[ii+1] >= bytes) {
            return 1 + ii;
        }
        if(block_sizes[ii+2] >= bytes) {
            return 2 + ii;
        }
        if(block_sizes[ii+3] >= bytes) {
            return 3 + ii;
        }
    }
    return NUM_BUCKETS;
}

// for a given size find how many entries can fit with used
// flags and meta data, into an alignment size.
static
inline
size_t
num_entries_in_aligned(size_t bytes)
{
    // calculate the per entry size including the bit in the used
    // field
    size_t bits_per_entry  = (bytes << 3) + 1;

    // get the amount of space available for the data block and used field
    size_t entry_space = ALIGNMENT - sizeof(block_md);

    // fit sets of entries into alloted space
    size_t num_entries = (entry_space<<3) / bits_per_entry;
    return num_entries;
}

// just like num_entries_in_aligned, but returns the number of used chars instead.
// It is possible that there are less entries then bits in the bitfield. since
// we have to have multiples of 8 for the bitfield
static
inline
size_t
num_used_bytes_aligned(size_t bytes)
{
    // calculate the per entry size
    size_t bits_per_entry  = (bytes << 3) + 1;

    // get the amount of space available for the data block and used field
    size_t entry_space = ALIGNMENT - sizeof(block_md);

    // fit sets of entries into alloted space
    size_t num_entries = (entry_space<<3) / bits_per_entry;

    size_t num_used_chars = num_entries >> 3;
    // round up one
    num_used_chars += ((num_used_chars << 3) < num_entries);
    return num_used_chars;
}

// determines at runtime, which arena this thread will 
// prioritize
static
inline
void
decide_arena(void)
{
    // don't want race conditions with initialization that's for sure
    pthread_mutex_lock(&init_lock);
    for(int ii = 0; ii < NUM_ARENAS; ++ii) {
        // find an arena that isn't being used
        if(arena_taken[ii] == 0) {
            // found one, initialize the threads arena info
            thread_arena = ii;
            arena_taken[ii] = 1;
            for(int yy = 0; yy< NUM_BUCKETS; ++yy) {
                locks[ii][yy] = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
            }
            break;
        }
    }
    // initialization done
    pthread_mutex_unlock(&init_lock);
    // in the case where all the arenas are taken use the last one
    if(thread_arena == -1) {
        // everyone after thread 16 gets the last arena
        thread_arena = NUM_ARENAS-1;
    }
}

// decides dynamically if this particular thread needs to (un)lock its arena
// There are times where shared arenas need to be locked when unshared arenas
// need not be
static
inline
void
lock_if_shared_arena(long index)
{
    if(thread_arena == NUM_ARENAS-1) {
        pthread_mutex_lock(&locks[NUM_ARENAS-1][index]);
    }
}

static
inline
void
unlock_if_shared_arena(long index)
{
    if(thread_arena == NUM_ARENAS-1) {
        pthread_mutex_unlock(&locks[NUM_ARENAS-1][index]);
    }
}

static
inline
void
lock_if_unshared_arena(long index)
{
    if(thread_arena < NUM_ARENAS-1) {
        pthread_mutex_lock(&locks[thread_arena][index]);
    }
}

static
inline
void
unlock_if_unshared_arena(long index)
{
    if(thread_arena < NUM_ARENAS-1) {
        pthread_mutex_unlock(&locks[thread_arena][index]);
    }
}
// allocates enough space for a block of entries while maintaining the alignment requirement
// This is the secret sauce that allows for the metadata for any entry to be obtained by just
// masking off the low bits of the address, making obtaining the metadata for any entry
// almost trivial. (and constant time (: )
static
inline
void*
get_page_aligned(void)
{
    // first try to just allocate ALIGNMENT bytes
    char* block = mmap(0, ALIGNMENT, PROT_READ | PROT_WRITE, 
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    
    assert(block != MAP_FAILED);

    // if the address is aligned we can just return it
    if(!((uintptr_t)block & (ALIGNMENT-1))) {
        return block;
    }
    
    // It is not aligned unmap it and try again with more complicated strategy
    else {
        int rv = munmap(block, ALIGNMENT);
        assert(0 == rv);
    }

    // need to allocate 2x the alignment requirement to ensure that is can become aligned
    block = mmap(0, 2*ALIGNMENT, PROT_READ | PROT_WRITE, 
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    assert(block != MAP_FAILED);

    // determine how much the alignment is off and unmap all the memory that isn't aligned
    size_t unaligned_bytes  = (ALIGNMENT) - ( (uintptr_t)block & ((ALIGNMENT)-1) );
    unaligned_bytes &= ALIGNMENT-1;
    char* aligned_addr = block + unaligned_bytes;
    
    
    // this frees the extra pages after the aligned section
    int rv = munmap(aligned_addr + ALIGNMENT, ALIGNMENT-unaligned_bytes);
    assert(0 == rv);

    // if block is aligned this will not free anything, otherwise
    // it frees the pages in front of the aligned section
    if(unaligned_bytes) {
        int rv = munmap(block, unaligned_bytes);
        assert(0 ==  rv);
    }

    // the block is now successfully aligned
    return aligned_addr;
}

// this allocates a much larger page with the specified minimum size
// similar to get_page_aligned() but this allocates for a given number
// of pages allowing for allocations of sizes other than the block allocation
static
inline
void*
map_aligned_large(size_t pages)
{
    size_t map_bytes = pages * PAGE_SIZE;

    // need to map in an additional ALIGNMENT bytes to ensure things get aligned
    char* block = mmap(0, ALIGNMENT+map_bytes, PROT_READ | PROT_WRITE, 
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    assert(block != MAP_FAILED);

    // determine how much the alignment is off and unmap all the memory that isn't aligned
    char* aligned_addr = block + ALIGNMENT-(((uintptr_t)block) & (ALIGNMENT-1));
    size_t unaligned_before = aligned_addr - block;
    size_t unaligned_after = (ALIGNMENT)- unaligned_before;

    // unmap pages before the block
    if(unaligned_before) {
        int rv = munmap(block, unaligned_before);
        assert(0 == rv);
    }

    // unmap the pages after the block
    if(unaligned_after) {
        int rv =  munmap(aligned_addr+map_bytes, unaligned_after);
        assert(0 == rv);
    }

    // address is now aligned
    return aligned_addr;
}

// checks if there are any reusable blocks in the threads
// arena, returns the block to be reused if one exists, 0
// otherwise, This allows memory that could be unmapped
// if it wasn't in use by another thread to be flagged
// for later use with an allocation of a different size
static
inline
block_md*
get_reusable()
{
    if(reusable_flag[thread_arena]==0) {
        return 0;
    }
    block_md* block = 0;
    for(int ii = 0; ii < NUM_BUCKETS; ++ii) {
        // are any of the buckets reusable
        // - it is marked reusable
        // AND
        // - there still is a block there
        // AND
        // - the block there is actually still not being used
        if(reusable[thread_arena][ii]) { 
            if (arenas[thread_arena][ii] &&
                arenas[thread_arena][ii]->references == 0) {
                // make sure this is no longer marked reusable
                reusable[thread_arena][ii] = 0;
                lock_if_unshared_arena(ii);
                // get the reusable block, and move the arena forward one
                block = arenas[thread_arena][ii];
                arenas[thread_arena][ii] = block->next;
                unlock_if_unshared_arena(ii);
                if(arenas[thread_arena][ii]){
                    arenas[thread_arena][ii]->prev = 0;
                }
                return block;
            }
            else {
                // This size bucket actually doesn't have a reusable block
                reusable[thread_arena][ii] = 0;
            }
        }
    }
    return block;
}

// unmaps large page allocations for blocks
static
inline
void
unmap_block(block_md* bb)
{
    // all of the chunks are of size ALIGNMENT
    if(0 != munmap(bb, ALIGNMENT)) {
        printf("failed to unmap at %s:%dn", __FILE__, __LINE__);
    } 
}


// Gets the pointer to the blocks metadata assuming
// the pointer was allocated using our strategy
static
inline
block_md*
get_block_md(void* addr)
{
    // ALIGNMENT = 0000 .... 0000 1000 0000 0000
    // ALIGNMENT-1 = 0000 .... 0000 0111 1111 1111
    size_t aligned_offset = (uintptr_t)addr & (ALIGNMENT-1);
    return (block_md*)((char*)addr - aligned_offset);
}

// gets pointer to the used bitfield of metadata
// In this case it's just an array of char
static
inline
char*
get_used_array(block_md* bb)
{
    // used byte array start right after the block_md information
    return (char*)((char*)bb + sizeof(block_md));
}

// initializes a new block's meta data using
// the specified size
static
inline
void
block_init(void* block, size_t size)
{
    // convert to block meta data
    block_md* bb = (block_md*)block;

    // set fields
    bb->size = size;
    bb->arena = thread_arena;
    bb->first_off = 0;
    bb->references = 0;
    bb->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    bb->next = 0;
    bb->prev = 0;
    // set all used fields to 0
    memset(get_used_array(bb), 0, num_used_bytes_aligned(size));
}

// determines if all chunk entries have been marked used, this is trivial
// since the first_off gives the index into the bit field of the first
// unused entry. if this is larger than the number of entries for the size
// then all of them must be in use
static
inline
int
block_all_used(block_md* bb)
{
    return bb->references >= num_entries_in_aligned(bb->size);
}

// sets the bit at ptr + bit_offset to 1 
static
inline
void
set_bit(char* ptr, size_t bit_offset)
{
    ptr[bit_offset>>3] |= 0x80 >> (bit_offset&7);
}

// same as set_bit, but instead clears it to 0
static
inline
void
clear_bit(char* ptr, size_t bit_offset)
{
    ptr[bit_offset>>3] &= ~(0x80 >> (bit_offset&7));
}

// gets the first unused block from bb returning the next 
// unused field
static
inline
size_t
get_next_unused(block_md* bb)
{
    // number of chars used to store used fields
    const int num_entries = num_entries_in_aligned(bb->size);
    char* used = get_used_array(bb);
    set_bit(used, bb->first_off);

    // apparently my program spends a lot of time here
    // so here is gross, probably poorly written optimization
    // code that makes my eyes bleed, the goal is to try and
    // make the checks aligned at byte boundaries so entire
    // chars can be checked at a time.

    // get the char index of the first available offset
    long index = bb->first_off >> 3;
    long bit_index = bb->first_off + 1;
    
    // get the number of bits away from byte alignment the bit offset is
    long offset = bit_index - (index << 3);
    char start = used[index];
    
    // check of the bits before aligning at the byte boundary
    // need fall through in this case to allow for this to handle
    // any range of offsets
    switch(offset) {
        case 1:
            if(~(start << 1) & 0x80) return bit_index - offset+1;
        case 2:
            if(~(start << 2) & 0x80) return bit_index - offset+2;
        case 3:
            if(~(start << 3) & 0x80) return bit_index - offset+3;
        case 4:
            if(~(start << 4) & 0x80) return bit_index - offset+4;
        case 5:
            if(~(start << 5) & 0x80) return bit_index - offset+5;
        case 6:
            if(~(start << 6) & 0x80) return bit_index - offset+6;
        case 7:
            if(~(start << 7) & 0x80) return bit_index - offset+7;
            bit_index += (8-offset);
    }
    // now we can check chars at a time for zeros, only then do we need to check
    // the individual bits
    for(; bit_index < num_entries; bit_index += 8) {
        if( (start = used[bit_index >> 3]) != -1) {
            if(~(start) & 0x80) return bit_index;
            if(~(start << 1) & 0x80) return bit_index+1;
            if(~(start << 2) & 0x80) return bit_index+2;
            if(~(start << 3) & 0x80) return bit_index+3;
            if(~(start << 4) & 0x80) return bit_index+4;
            if(~(start << 5) & 0x80) return bit_index+5;
            if(~(start << 6) & 0x80) return bit_index+6;
            if(~(start << 7) & 0x80) return bit_index+7;
        }
    }
    // this block is full
    return num_entries + 1;
}

// A block entries id is the index in the bit field that corresponds to it.
// This gets the entry of the block for the specified id
static
inline
void*
get_entry_from_id(block_md* bb, size_t entry_id)
{
    // need to calculate the length of the metadata with all of the used field bitmaps
    size_t md_bytes = sizeof(block_md) + num_used_bytes_aligned(bb->size) ;

    // the location of the entry is at size*id after the metadata and used fields
    size_t entry_offset = bb->size * entry_id;
    return ((char*)bb + md_bytes + entry_offset);
}

// Given a pointer to an entry in the block, compute the id
// Basically the reverse of get_entry_from_id()
static
inline
size_t
get_id_from_entry(block_md* bb, char* entry)
{
    // determine sizeof(block_md) + sizeof(used_array) 
    size_t md_bytes = sizeof(block_md) + num_used_bytes_aligned(bb->size) ;

    // the id of the entry must be entry - metadata bytes with used array / size
    size_t entry_offset = (entry - (char*)bb) - md_bytes;
    return entry_offset / bb->size;
}

// gets a memory allocation from a bucket allocating another one if
// needed, a bucket being a collection of blocks of the same size allocation
// Takes a reference to the bucket to use since it might need to alter
// the bucket start pointer.
static
inline
void*
bucket_get(block_md** bb, size_t size)
{
    // don't have to lock here because only this thread can change the
    // start node of its arena (unless it shared, but that's already
    // handled) so we don't have to lock unless we want to use
    // the next blocks

    // Do we have a bucket that is currently being used to satisfy requests
    // of this size?
    if(!(*bb)) {
        // Ok, we don't have a block
        // see if there are any reusable blocks in the threads arena
        *bb = get_reusable();
        if(!(*bb)) {
            // no reusable blocks, fall back to mmap-ing one in
            *bb = get_page_aligned();
        }
        
        // initialize this block
        block_init(*bb, size);
    }
    // lock this block so nobody can change it once we start using it  
    block_md* block_in_use = *bb;
    pthread_mutex_lock(&(block_in_use->lock));

    // return the first entry that is unused, doing the book keeping
    void* retval = get_entry_from_id(*bb, (*bb)->first_off);
    (*bb)->first_off = get_next_unused(*bb); 
    (*bb)->references += 1; 

    // if this allocation caused the block to be full, set the arena to use
    // the next one in the list
    if(block_all_used(*bb)) {
        // if there a block after this one we need to grab a lock,
        // this ensures that anyone might be working on freeing with the
        // next block doesn't cause a race condition
        lock_if_unshared_arena(get_size_index(size));
        if((*bb)->next) {
            (*bb) = (*bb)->next;
            (*bb)->prev = 0;
        } 
        else {
            (*bb) = 0;
        }
        unlock_if_unshared_arena(get_size_index(size));
    }
    pthread_mutex_unlock(&(block_in_use->lock));
    
    // give this entry to the user
    return retval; 
}

// claims a block that just had its first free by adding it to an
// arenas block list
static
inline
void
claim_block(block_md* bb)
{ 
    // need to know what block list this goes in
    long size_index = get_size_index(bb->size);
    bb->arena = thread_arena;
    lock_if_unshared_arena(size_index);
    // add block to arena
    if(arenas[thread_arena][size_index]) {
        arenas[thread_arena][size_index]->prev = bb;
        bb->next = arenas[thread_arena][size_index];
    }
    else {
        bb->next = 0;
    }
    bb->prev = 0;
    arenas[thread_arena][size_index] = bb;
    unlock_if_unshared_arena(size_index);
}

// remove a block owned by this threads arena
static
inline
void
remove_owned(block_md* bb)
{
    long size_index = get_size_index(bb->size);

    // check if it is the head node of the arenas, 
    // moving it forward if need be
    lock_if_unshared_arena(size_index);
    if(arenas[bb->arena][size_index] == bb) {
        arenas[bb->arena][size_index] = bb->next;
        if(arenas[bb->arena][size_index]) {
            arenas[bb->arena][size_index]->prev=0;
        }
    }
    else {
        // handle removeing node from doubly linked list
        if(bb->prev) {
            bb->prev->next = bb->next;
        }
        if(bb->next) {
            bb->next->prev = bb->prev;
        }
    }
    unlock_if_unshared_arena(size_index);
    pthread_mutex_unlock(&(bb->lock));
    unmap_block(bb);
}

// does what it can to remove a node from a different
// arena than what this thread uses
static
inline
void
remove_unowned(block_md* bb)
{
    // need the index of arenas used for this size
    long size_index = get_size_index(bb->size);
    pthread_mutex_lock(&locks[bb->arena][size_index]);

    // if this is the head node of the other threads arena
    // don't move it, that thread could be using it
    if(arenas[bb->arena][size_index] != bb) {
        // But we can change the list after the head because
        // the thread needs to lock its arena before moving 
        // head
        if(bb->prev) {
            bb->prev->next = bb->next;
        }
        if(bb->next) {
            bb->next->prev = bb->prev;
        } 
        pthread_mutex_unlock(&(locks[bb->arena][size_index]));
        pthread_mutex_unlock(&(bb->lock));
        pthread_mutex_destroy(&(bb->lock));
        // unmap the block
        unmap_block(bb);
    }
    else {
        // couldn't unmap it because another thread's arena is looking at the block
        // flag to the other thread that the block is reusable
        reusable[bb->arena][size_index] = 1;
        reusable_flag[bb->arena] = 1;
        pthread_mutex_unlock(&locks[bb->arena][size_index]);
        pthread_mutex_unlock(&(bb->lock));
    }
}

// frees the data corresponding to addr by doing the following bookeeping
//  - set used bit to 0
//  - moving offset to first available entry to this just free'd entry
//    if it's id is lower
//  - decrements the amount of references to the block
//  - If it can, this will also unmap the block for future allocations
//    otherwise it will mark the block as reusable
static 
inline
void
bucket_free(block_md* bb, void* addr)
{
    // find the start of the block based on ALIGNMENT
    char* used = get_used_array(bb);

    // get ID of free'd entry
    size_t id = get_id_from_entry(bb, addr);

    // need to take the lock before editing
    pthread_mutex_lock(&(bb->lock));
    // mark the entry as unused, also decrement the number of refs
    clear_bit(used, id);
    bb->references -= 1;

    // if the id that was just freed is before first offset, update offset
    if(id < bb->first_off) {
        bb->first_off = id;
    }
    // if the block was previously full, we can use it for future allocations
    // since whatever thread was using it should have seen it was full and removed
    // its references to this block in its arena
    if(bb->references == num_entries_in_aligned(bb->size)-1) {
        claim_block(bb);
    }

    // Since we are reference counting as well, if the current number of
    // references is 0, we can safely unmap this block
    else if(bb->references == 0 ) {
        // if this node is in this threads arena
        if(bb->arena == thread_arena) {
            remove_owned(bb); 
            return;
        }
        else {
            // different strategy if the node is from a different
            // arena
            remove_unowned(bb);
            return;
        }
                
    }
    pthread_mutex_unlock(&(bb->lock));
}

// allocate using the block strategy
void*
xmalloc(size_t bytes)
{
    // check if this thread has decided on an arena 
    if(thread_arena == -1) {
        decide_arena();
    }
    void* alloc = 0;

    // if requested size is greater than largest bucket, then return
    // an mmaped region aligned at ALIGNMENT instead. This ensures that
    // the size can be obtained with the same methods as bucket allocation
    if(bytes >= block_sizes[NUM_BUCKETS-1]) {
        // number of bytes needed to allocate is the requested size
        // and the header
        size_t num_bytes = (bytes) + sizeof(header);

        // get the number of pages needed for this size
        size_t num_pages = num_bytes / PAGE_SIZE;
        num_pages += (num_bytes > num_pages * PAGE_SIZE); // round up

        // check to see if this is still smaller than a block allocation,
        // maybe we can use a reusable block
        header* block = 0;
        if(num_bytes <= ALIGNMENT) {
               block = (header*)get_reusable();
        }
        // regardless, if there is no block mmap enough bytes
        if(!block) {
            block = map_aligned_large(num_pages);
        }
        block->size = num_pages * PAGE_SIZE;
        block->arena = thread_arena;
        alloc = ((char*)block+sizeof(header));
    }
    else {
        // get the index used for the size requested
        long size_index = get_size_index(bytes); 
        
        // a lot of bucket grow quickly, but then stop growing
        if(bytes >= ivec_init_size && bytes <= 256) {
            size_index += 2;
        }

        // conditionally lock the arena
        lock_if_shared_arena(size_index);


        // get allocation from bucket
        alloc = bucket_get(&(arenas[thread_arena][size_index]), block_sizes[size_index]);

        // conditionally unlock the arena
        unlock_if_shared_arena(size_index);
    }
    return alloc;
}

void
xfree(void* ptr)
{
    // get the block metadata from the pointer 
    block_md* bb = get_block_md(ptr);
    long size_index = get_size_index(bb->size);

    // if the offset is equal to that of a large MMAPd region
    // just munmap it and call it a day, no lock contention here
    // since only one pointer is in this mmapd location
    if(bb->size > block_sizes[NUM_BUCKETS-1]){
        header* info = (header*)((char*)ptr - sizeof(header));
        int rv = munmap(info, info->size);
        assert(0 == rv);
        return;
    }
    // conditionally lock arena
    lock_if_shared_arena(size_index);

    // this must be a bucket entry
    bucket_free(bb, ptr);

    // conditionally unlock the arena
    unlock_if_shared_arena(size_index);
}

void*
xrealloc(void* prev, size_t bytes)
{
    // xrealloc is a mutant beast that has 3 different behaviors
    // 1. prev is 0, that means malloc
    // 2. bytes is 0, that means free
    // 3. neither is zero, that means expand or shrink
    if(!prev) {
        return xmalloc(bytes);
    } 
    else if (!bytes) {
        xfree(prev);
        return 0;
    }
    else {
        // get information from the block pointer
        block_md* bb = get_block_md(prev);
        // if the size of the allocated block is enough for
        // the new number of bytes just return it again
        if(bb->size >= bytes && bytes*3 > bb->size) {
            return prev;
        }

        // bytes to copy is the min of bytes and bb->size
        size_t bytes_to_copy = bytes;
        if(bb->size < bytes) {
            bytes_to_copy = bb->size;
        }

        // allocate new memory 
        void* alloc = xmalloc(bytes);

        // copy old data to new location
        memcpy(alloc, prev, bytes_to_copy);

        // free old data;
        xfree(prev);

        // return new data
        return alloc;
    }
}

