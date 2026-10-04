/*
NSPIRE_THREADS.C

Cooperative threads for the Nspire port, and the part of POSIX threads the
platform layer shared with the Linux port uses (xbox_kernel.c).

Ndless programs have one thread and no scheduler. The game starts a few
threads of its own (input polling; port/nspire/README.md) that mostly wait,
so they run here as coroutines on their own stacks: a thread runs until it
waits, sleeps or yields, and then the next one in the ring does. Waits poll
their condition between switches. Mutexes are recursive, and condition
variables wake every waiter (spurious wakeups are allowed).

Thread-local storage is emulated (clang -femulated-tls): each thread keeps
its own table of the game's __thread variables.
*/

#include "nspire.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MINIMUM_STACK_SIZE 0x4000
#define MAXIMUM_STACK_SIZE 0x10000
#define DEFAULT_STACK_SIZE 0x8000

struct nspire_thread
{
	/* the saved stack pointer while switched out (nspire_switch.S) */
	unsigned long *stack_pointer;
	void *(*start)(void *);
	void *argument;
	void *stack;
	unsigned long stack_size;
	int finished;
	pthread_t identifier;
	/* emulated thread-local storage, by variable index */
	void **locals;
	unsigned long local_count;
	struct nspire_thread *next;
};

void nspire_thread_switch(unsigned long **save_stack, unsigned long *load_stack);

static struct nspire_thread main_thread = { 0, 0, 0, 0, 0, 0, 1, 0, 0, &main_thread };
static struct nspire_thread *current_thread = &main_thread;
static pthread_t next_identifier = 2;

/* ---------- scheduling */

void nspire_yield(void)
{
	struct nspire_thread *previous = current_thread;
	struct nspire_thread *next = previous->next;

	while (next->finished && next != previous)
		next = next->next;
	if (next == previous)
		return;
	current_thread = next;
	nspire_thread_switch(&previous->stack_pointer, next->stack_pointer);
}

int nspire_thread_count(void)
{
	struct nspire_thread *thread = &main_thread;
	int count = 0;

	do
	{
		count += !thread->finished;
		thread = thread->next;
	}
	while (thread != &main_thread);
	return count;
}

static void thread_entry(void)
{
	struct nspire_thread *thread = current_thread;

	thread->start(thread->argument);
	thread->finished = 1;
	/* the stack stays until the program exits: this thread is still on it */
	for (;;)
		nspire_yield();
}

int sched_yield(void)
{
	nspire_yield();
	return 0;
}

/* ---------- threads */

int pthread_attr_init(pthread_attr_t *attributes)
{
	memset(attributes, 0, sizeof(*attributes));
	attributes->is_initialized = 1;
	attributes->stacksize = DEFAULT_STACK_SIZE;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
	attributes->is_initialized = 0;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	attributes->detachstate = state;
	return 0;
}

/* The Linux port asks for 1 MB stacks; the calculator has 25 MB in all, and
the game's own threads were written for the Xbox's 16 KB. */
int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	attributes->stacksize = size < MINIMUM_STACK_SIZE ? MINIMUM_STACK_SIZE
		: size > MAXIMUM_STACK_SIZE ? MAXIMUM_STACK_SIZE : size;
	return 0;
}

int pthread_create(pthread_t *identifier, const pthread_attr_t *attributes,
	void *(*start)(void *), void *argument)
{
	struct nspire_thread *thread = calloc(1, sizeof(*thread));
	unsigned long size = attributes && attributes->stacksize ? attributes->stacksize : DEFAULT_STACK_SIZE;
	unsigned long *top;

	if (!thread)
		return EAGAIN;
	thread->stack = malloc(size);
	if (!thread->stack)
	{
		free(thread);
		return EAGAIN;
	}
	thread->stack_size = size;
	thread->start = start;
	thread->argument = argument;
	thread->identifier = next_identifier++;

	/* the frame nspire_thread_switch pops: r4-r11 and the return address,
	leaving the stack 8-byte aligned (AAPCS) at thread_entry */
	top = (unsigned long *)(((unsigned long)thread->stack + size) & ~7UL);
	thread->stack_pointer = top - 11;
	memset(thread->stack_pointer, 0, 11 * sizeof(unsigned long));
	thread->stack_pointer[8] = (unsigned long)thread_entry;

	thread->next = current_thread->next;
	current_thread->next = thread;
	if (identifier)
		*identifier = thread->identifier;
	return 0;
}

pthread_t pthread_self(void)
{
	return current_thread->identifier;
}

int pthread_equal(pthread_t first, pthread_t second)
{
	return first == second;
}

/* ---------- mutexes

A pthread_mutex_t is a pointer to one of these; PTHREAD_MUTEX_INITIALIZER
(newlib: all bits set) and zero are made on first use. */

struct nspire_mutex
{
	pthread_t owner;
	unsigned long count;
};

static struct nspire_mutex *mutex_get(pthread_mutex_t *mutex)
{
	if (*mutex == 0 || *mutex == (pthread_mutex_t)0xFFFFFFFFUL)
		*mutex = (pthread_mutex_t)(unsigned long)calloc(1, sizeof(struct nspire_mutex));
	return (struct nspire_mutex *)(unsigned long)*mutex;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attributes)
{
	memset(attributes, 0, sizeof(*attributes));
	attributes->is_initialized = 1;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes)
{
	attributes->is_initialized = 0;
	return 0;
}

/* every mutex is recursive */
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
	(void)attributes;
	(void)type;
	return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	*mutex = 0;
	return mutex_get(mutex) ? 0 : ENOMEM;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	if (*mutex != 0 && *mutex != (pthread_mutex_t)0xFFFFFFFFUL)
		free((void *)(unsigned long)*mutex);
	*mutex = 0;
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	struct nspire_mutex *state = mutex_get(mutex);

	if (state->count && state->owner != current_thread->identifier)
		return EBUSY;
	state->owner = current_thread->identifier;
	state->count++;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	while (pthread_mutex_trylock(mutex) == EBUSY)
		nspire_yield();
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	struct nspire_mutex *state = mutex_get(mutex);

	if (!state->count || state->owner != current_thread->identifier)
		return EPERM;
	state->count--;
	return 0;
}

/* ---------- condition variables

A pthread_cond_t is a generation count: waiters wait for it to change. */

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	*condition = 0;
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	(void)condition;
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	/* never the initializer's value */
	if (++*condition == (pthread_cond_t)0xFFFFFFFFUL)
		*condition = 0;
	return 0;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	return pthread_cond_broadcast(condition);
}

static int condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct nspire_mutex *state = mutex_get(mutex);
	pthread_cond_t generation = *condition;
	unsigned long count = state->count;
	int result = 0;

	state->count = 0;
	while (*condition == generation)
	{
		if (deadline)
		{
			struct timespec now;

			clock_gettime(CLOCK_REALTIME, &now);
			if (now.tv_sec > deadline->tv_sec ||
				(now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
			{
				result = ETIMEDOUT;
				break;
			}
		}
		nspire_yield();
	}
	while (state->count && state->owner != current_thread->identifier)
		nspire_yield();
	state->owner = current_thread->identifier;
	state->count = count;
	return result;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	return condition_wait(condition, mutex, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	return condition_wait(condition, mutex, deadline);
}

/* ---------- emulated thread-local storage (clang -femulated-tls)

The compiler describes each __thread variable with one of these and asks
for its address in the calling thread. */

struct __emutls_control
{
	unsigned long size;
	unsigned long align;
	union
	{
		unsigned long index;
		void *address;
	} object;
	void *value;
};

static unsigned long emulated_tls_variable_count;

void *__emutls_get_address(struct __emutls_control *control)
{
	struct nspire_thread *thread = current_thread;
	unsigned long index = control->object.index;
	void *local;

	if (!index)
		index = control->object.index = ++emulated_tls_variable_count;
	if (index > thread->local_count)
	{
		void **locals = realloc(thread->locals, index * sizeof(void *));

		if (!locals)
			abort();
		memset(locals + thread->local_count, 0, (index - thread->local_count) * sizeof(void *));
		thread->locals = locals;
		thread->local_count = index;
	}
	local = thread->locals[index - 1];
	if (!local)
	{
		unsigned long align = control->align > sizeof(void *) ? control->align : sizeof(void *);
		unsigned char *block = malloc(control->size + align);

		/* never freed: the variables live as long as the program */
		if (!block)
			abort();
		local = (void *)(((unsigned long)block + align - 1) & ~(align - 1));
		if (control->value)
			memcpy(local, control->value, control->size);
		else
			memset(local, 0, control->size);
		thread->locals[index - 1] = local;
	}
	return local;
}
