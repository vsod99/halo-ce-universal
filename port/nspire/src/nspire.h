/*
NSPIRE.H

Internals shared by the Nspire port's platform files (port/nspire/src).
*/

#ifndef __HALO_NSPIRE_H
#define __HALO_NSPIRE_H

/* ---------- threads (nspire_threads.c) */

/* runs the other cooperative threads until each waits or yields */
void nspire_yield(void);
int nspire_thread_count(void);

/* ---------- time (nspire_time.c) */

void nspire_time_initialize(void);
void nspire_time_dispose(void);
/* 32768 Hz ticks since nspire_time_initialize */
unsigned long long nspire_ticks(void);

/* ---------- the heap (nspire_heap.c): freed at exit */

void nspire_heap_initialize(void);
void nspire_heap_statistics(unsigned long *blocks, unsigned long *bytes);

/* ---------- memory (nspire_memory.c) */

/* maps the Xbox memory window at 0x80000000; returns 0 when the calculator
has too little memory */
int nspire_memory_initialize(void);
void nspire_memory_dispose(void);
/* what malloc can still give */
unsigned long nspire_memory_heap_free(void);
/* bytes of the window still free, and in all */
unsigned long nspire_memory_window_free(void);
unsigned long nspire_memory_window_size(void);

/* the pager's pages: reserved addresses whose coarse tables exist, left
unmapped (nspire_paging.c maps them); 0 on failure */
int nspire_memory_reserve_paged(unsigned long address, unsigned long size);
volatile unsigned long *nspire_memory_page_entry(unsigned long address);

/* ---------- paging (nspire_paging.c) */

/* nspire_paging_fault's answers */
enum
{
	NSPIRE_PAGING_NOT_OURS = 0,
	NSPIRE_PAGING_DONE,
	/* the block must be read from the file (nspire_paging_service) */
	NSPIRE_PAGING_NEEDS_READ,
};

struct nspire_paging_statistics
{
	unsigned long faults;
	unsigned long reads;
	unsigned long evictions;
	unsigned long pinned_blocks;
	unsigned long anonymous_blocks;
	unsigned long inflate_failures;
	unsigned long out_of_slots;
	unsigned long slots;
	unsigned long slots_used;
};

void *nspire_paging_reserve(unsigned long address, unsigned long size);
/* the data abort handler's part (nspire_exceptions.c): NSPIRE_PAGING_* */
int nspire_paging_fault(unsigned long address, unsigned long status, unsigned long pc);
/* reads the block at address in and maps it, outside the exception
(nspire_abort.S's trampoline); nonzero on success */
int nspire_paging_service(unsigned long address);
/* whether an address is the pager's, and bringing its page in if the pool
has it (from the exception handlers): NSPIRE_PAGING_* */
int nspire_paging_is_paged(unsigned long address);
int nspire_paging_make_resident(unsigned long address);
/* the map file blocks are read from, and where each block is in it */
int nspire_paging_set_source(const char *path);
int nspire_paging_hook(void);
void nspire_paging_unhook(void);
unsigned long nspire_paging_allocate_pool(unsigned long bytes);
void nspire_paging_set_block(unsigned long address, unsigned long file_offset, unsigned long size);
void nspire_paging_drop(unsigned long address, unsigned long size);
void nspire_paging_get_statistics(struct nspire_paging_statistics *statistics);
void nspire_paging_dispose(void);

/* ---------- exceptions (nspire_exceptions.c) */

#define NSPIRE_ALIGNMENT_SITES 16

struct nspire_alignment_statistics
{
	/* unaligned accesses done byte by byte, and the first places they were */
	unsigned long fixes;
	unsigned long site_count;
	unsigned long sites[NSPIRE_ALIGNMENT_SITES];
};

void nspire_alignment_get_statistics(struct nspire_alignment_statistics *statistics);

/* ---------- input (xinput_nspire.c) */

/* the controller as last read, for the status line */
void nspire_input_describe(char *text, unsigned long size);

/* ---------- frame timing (nspire_profile.c) */

void nspire_profile_begin(long section);
void nspire_profile_end(long section);
void nspire_profile_report(unsigned long frames);

/* ---------- the screen (posix_video.c) */

#define NSPIRE_SCREEN_WIDTH 320
#define NSPIRE_SCREEN_HEIGHT 240

int nspire_video_initialize(void);
void nspire_video_release(void);
/* the screen's 320x240 RGB565 pixels, row after row */
unsigned short *nspire_video_pixels(void);
void nspire_video_text(int x, int y, const char *text);
void nspire_video_present(void);

/* ---------- files */

/* closes every file still open (posix_nspire.c), at exit */
void nspire_close_all_files(void);
/* a descriptor for reading, or -1; and size bytes at offset, or -1 */
int nspire_file_open_read(const char *path);
void nspire_file_close(int descriptor);
long nspire_file_read_at(int descriptor, unsigned long offset, void *buffer, unsigned long size);

/* ---------- files (nspire_main.c) */

/* the folder the program was started from, which holds the converted map */
const char *nspire_program_directory(void);
/* the level the program plays: from its name, halo_<level>.tns (b30 otherwise) */
const char *nspire_level_name(void);

/* ---------- log (nspire_main.c) */

/* appends to halo_log.txt.tns next to the program, and flushes */
void nspire_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* shows the message in a dialog, writes it to the log and exits */
void nspire_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
