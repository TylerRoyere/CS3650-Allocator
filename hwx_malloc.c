
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "xmalloc.h"

typedef struct hm_stats {
    long pages_mapped;
    long pages_unmapped;
    long chunks_allocated;
    long chunks_freed;
    long free_length;
} hm_stats; 

// Node in the free list
typedef struct free_list_node {
    size_t size;
    struct free_list_node* next;
} free_list_node;

// global freed memory state
static free_list_node* free_list = 0;

// lock for the global free list
pthread_mutex_t list_lock;

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.

long
free_list_length()
{
    // TODONE: Calculate the length of the free list.
    
    free_list_node* curr = free_list;
    // Get length by counting nodes till ptr is NULL 
    int length = 0;
    while(curr) {
        curr = curr->next;
        ++length;
    }

    return length;
}

// returns a block big enough for the request
// if no blocks big enough, return NULL
static 
free_list_node*
find_node_size(size_t size)
{
    // loop through the list looking for entry large enough
    free_list_node** curr = &free_list;

    while(*curr && (*curr)->size < size) {
        curr = &(*curr)->next;
    }

    // if *curr is a valid pointer we need to remove memory from the list
    free_list_node* block = 0;
    if(curr && *curr) {
        // curr is pointing to the "next" pointer that points to the block
        // we want
        block = *curr;
        *curr = (*curr)->next; // need to make sure last next = 0 when adding 
    }

    // either a block was found that's big enough whose located at curr
    // or curr is null -> no block found
    return block;
}

// adds a node to the free list maintaining two invariants
// - The Free List is sorted by the address of the block
// - Two adjacent blocks, will be coalesced (joined) into a bigger block
// This returns a pointer to the list after trying to add node to it
static
free_list_node*
free_list_add(free_list_node* start, free_list_node* node)
{
    // if there is no list to add to, return the node
    if(!start) {
        node->next = 0;
        return node;
    }

    // determine strategy based on relative position of start and node
    if(start < node) {
        // node is in higher memory than start
        // first add the node to the next in the list
        start->next = free_list_add(start->next, node);

        // determine if the start node overlaps with the new next node -> join
        char* start_node_end_byte = ((char*)start + start->size);
        if(start->next && start_node_end_byte >= (char*)start->next) {
            // the next node and this one can be coalesced
            start->size += start->next->size;
            start->next = start->next->next;
        } 
        // regardless return start
        return start;
    }
    else {
        // this must be the first cell thats in higher memory than the node
        // determine if they form a contiguous region
        char* node_end_byte = (char*)node + node->size;
        if(node_end_byte >= (char*)start) {
            // start and node can be coalesced
            node->size += start->size;
            node->next = start->next;
        }
        else {
            // prepend node to the start node
            node->next = start;
        }
        return node;
    }
}


hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

void*
xmalloc(size_t size)
{
    stats.chunks_allocated += 1;
    size += sizeof(size_t);

    // Blocks cannot be smaller than free list node or 
    // else we cannot make a free list entry
    if(size < sizeof(free_list_node)) {
        size = sizeof(free_list_node);
    }
    
    free_list_node* alloc = 0; 
    // first determine if block is less than one page
    if(size < PAGE_SIZE) {
        // see if there is a big enough block on the free list
        pthread_mutex_lock(&list_lock);
        free_list_node* block = find_node_size(size);
        pthread_mutex_unlock(&list_lock);
        
        // check if a block was found
        if(!block) {
            // block was not found need to mmap 
            block = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            block->size = PAGE_SIZE;

            // increment pages mapped stats
            stats.pages_mapped += 1;
        }

        // if the block is larger than needed, try to make extra space into
        // new free list cell
        long left_over = block->size - (size + sizeof(free_list_node));
        if(left_over >= 0) {
            // the block can be split in two, one to be returned to
            // the user, the other to be added to the free list

            // place whatever is left over into the free_list
            free_list_node* block_extra = (free_list_node*)((char*)block + size);
            block_extra->size = block->size - size;

            // use mutex to make this thread safe
            pthread_mutex_lock(&list_lock);
            free_list = free_list_add(free_list, block_extra);
            pthread_mutex_unlock(&list_lock);

            // update the block size
            block->size = size;
        }
        // block is now definitely large enough to satisfy the request
        // it's either the size requested or larger by < sizeof(free_list_node)
        *(size_t*)block = block->size;
        alloc = (void*)((char*)block + sizeof(size_t));
    }
    else {
        // this is larger than a page
        // Determine the number of pages required
        int num_pages = div_up(size, PAGE_SIZE);
        
        // mmap the number of page
        char* block = mmap(0, num_pages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        stats.pages_mapped += num_pages;
        
        // The user will get a pointer into the block after the size
        *((size_t*)block) = num_pages * PAGE_SIZE;
        alloc = (void*)(block + sizeof(size_t));
    }

    return alloc;
}

// bad realloc implementation
void*
xrealloc(void* ptr, size_t size)
{
    // if size is 0, realloc becomes free
    if(size == 0) {
        xfree(ptr);
        return 0;
    }
    // if ptr is 0, realloc becomes malloc
    else if (!ptr) {
        return xmalloc(size);
    }
    else {
        // try to reallocate the memory
        
        // get the blocks address
        free_list_node* block = (free_list_node*)((char*)ptr - sizeof(size_t));
        size_t block_data_size = block->size - sizeof(size_t);

        // determine if the block has enough size for the requested allocation
        if(block_data_size < size) {
            // the block is too small, free this block and
            // allocate a new one with enough size
            char* new = xmalloc(size);

            // copy all of the bytes needed
            memcpy(new, ptr, block_data_size);

            // free the old node 
            xfree(ptr);
            return new;
        }
        else {
            // there is enough space for just return the original node
            return ptr;
        }
    }
}

void
xfree(void* item)
{
    stats.chunks_freed += 1;

    // shouldn't do anything if the item is NULL
    if(!item) {
        return;
    }

    // get the location of the node
    free_list_node* remove = (free_list_node*)((char*)(item) - sizeof(size_t));

    // if the item is smaller than a page, free_list time
    if(remove->size < PAGE_SIZE) {
        pthread_mutex_lock(&list_lock);
        free_list = free_list_add(free_list, remove); 
        pthread_mutex_unlock(&list_lock);
    }
    else {
        // larger than a page, just munmap it
        munmap(item, remove->size);
        stats.pages_unmapped += remove->size / PAGE_SIZE;
    }
}

