/*
WIN32_THREADS.C

The POSIX threads, mutexes, condition variables, clocks and sleeping the
platform layer shared with Linux uses, implemented with Windows (the headers
are in port/windows/include/posix). The original Xbox build shares this
file: nxdk's Windows API has every call it makes (tools/xbox_build.py).
*/

#include <windows.h>
#include <errno.h>
#include <stdlib.h>

#include "pthread.h"
#include "sched.h"
#include "time.h"

/* ---------- threads */

struct thread_start
{
	void *(*start)(void *);
	void *argument;
};

static DWORD WINAPI thread_main(LPVOID parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;

	free(parameter);
	start.start(start.argument);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	struct thread_start *parameter = malloc(sizeof(*parameter));
	DWORD identifier;
	HANDLE handle;

	if (!parameter)
		return EAGAIN;
	parameter->start = start;
	parameter->argument = argument;
	handle = CreateThread(NULL, attributes ? attributes->stack_size : 0, thread_main, parameter,
		STACK_SIZE_PARAM_IS_A_RESERVATION, &identifier);
	if (!handle)
	{
		free(parameter);
		return EAGAIN;
	}
	/* nothing joins threads: the handle is not needed */
	CloseHandle(handle);
	*thread = identifier;
	return 0;
}

int pthread_detach(pthread_t thread)
{
	(void)thread;
	return 0;
}

pthread_t pthread_self(void)
{
	return GetCurrentThreadId();
}

int pthread_equal(pthread_t thread1, pthread_t thread2)
{
	return thread1 == thread2;
}

int pthread_attr_init(pthread_attr_t *attributes)
{
	attributes->stack_size = 0;
	attributes->detached = 0;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	attributes->detached = state;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	attributes->stack_size = size;
	return 0;
}

/* ---------- mutexes */

int pthread_mutexattr_init(pthread_mutexattr_t *attributes)
{
	attributes->type = PTHREAD_MUTEX_DEFAULT;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
	attributes->type = type;
	return 0;
}

/* the critical section behind a mutex, created on first use so that
PTHREAD_MUTEX_INITIALIZER can be a constant */
static CRITICAL_SECTION *mutex_section(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (!section)
	{
		CRITICAL_SECTION *created = malloc(sizeof(*created));

		if (!created)
			abort();
		InitializeCriticalSection(created);
		section = InterlockedCompareExchangePointer(&mutex->section, created, NULL);
		if (section)
		{
			DeleteCriticalSection(created);
			free(created);
		}
		else
		{
			section = created;
		}
	}
	return section;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	mutex->section = NULL;
	mutex_section(mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (section)
	{
		DeleteCriticalSection(section);
		free(section);
		mutex->section = NULL;
	}
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	EnterCriticalSection(mutex_section(mutex));
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	return TryEnterCriticalSection(mutex_section(mutex)) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	LeaveCriticalSection(mutex_section(mutex));
	return 0;
}

/* ---------- condition variables */

#ifdef NXDK
/* nxdk's CONDITION_VARIABLE holds two events and is larger than a pointer:
the one behind a condition is created on first use, as a mutex's critical
section is (PTHREAD_COND_INITIALIZER stays a constant) */
static PCONDITION_VARIABLE condition_variable(pthread_cond_t *condition)
{
	PCONDITION_VARIABLE variable = condition->variable;

	if (!variable)
	{
		PCONDITION_VARIABLE created = malloc(sizeof(*created));

		if (!created)
			abort();
		InitializeConditionVariable(created);
		variable = InterlockedCompareExchangePointer(&condition->variable, created, NULL);
		if (variable)
		{
			UninitializeConditionVariable(created);
			free(created);
		}
		else
		{
			variable = created;
		}
	}
	return variable;
}

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	condition->variable = NULL;
	condition_variable(condition);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	PCONDITION_VARIABLE variable = condition->variable;

	if (variable)
	{
		UninitializeConditionVariable(variable);
		free(variable);
		condition->variable = NULL;
	}
	return 0;
}
#else
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "pthread_cond_t holds a CONDITION_VARIABLE");

/* the CONDITION_VARIABLE is the pthread_cond_t's pointer (zero is its
initial value) */
#define condition_variable(condition) ((PCONDITION_VARIABLE)&(condition)->variable)

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	InitializeConditionVariable(condition_variable(condition));
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	(void)condition;
	return 0;
}
#endif

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	SleepConditionVariableCS(condition_variable(condition), mutex_section(mutex), INFINITE);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct timespec now;
	long long milliseconds;

	clock_gettime(CLOCK_REALTIME, &now);
	milliseconds = ((long long)deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (milliseconds < 0)
		milliseconds = 0;
	if (!SleepConditionVariableCS(condition_variable(condition), mutex_section(mutex),
		milliseconds >= INFINITE ? INFINITE - 1 : (DWORD)milliseconds))
	{
		return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
	}
	return 0;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	WakeConditionVariable(condition_variable(condition));
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	WakeAllConditionVariable(condition_variable(condition));
	return 0;
}

/* ---------- time */

#ifdef NXDK
/* the wall clock: nxdk has the precise one and no modules to look it up in */
static void system_time(FILETIME *now)
{
	GetSystemTimePreciseAsFileTime(now);
}
#else
typedef VOID (WINAPI *system_time_proc)(LPFILETIME);

/* the wall clock: GetSystemTimePreciseAsFileTime where Windows has it
(Windows 8 and later), else GetSystemTimeAsFileTime. Importing the precise
one directly would keep the game from starting on Windows 7. Threads that
race here store the same pointer. */
static void system_time(FILETIME *now)
{
	static system_time_proc volatile proc;
	system_time_proc get_time = proc;

	if (!get_time)
	{
		get_time = (system_time_proc)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
			"GetSystemTimePreciseAsFileTime");
		if (!get_time)
			get_time = GetSystemTimeAsFileTime;
		proc = get_time;
	}
	get_time(now);
}
#endif

int clock_gettime(clockid_t clock, struct timespec *time)
{
	if (clock == CLOCK_MONOTONIC)
	{
		static LARGE_INTEGER frequency;
		LARGE_INTEGER counter;

		if (!frequency.QuadPart)
			QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		time->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
		time->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
	}
	else
	{
		/* 100 ns intervals since 1601 */
		FILETIME now;
		unsigned long long intervals;

		system_time(&now);
		intervals = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
		intervals -= 116444736000000000ULL;
		time->tv_sec = (time_t)(intervals / 10000000ULL);
		time->tv_nsec = (long)(intervals % 10000000ULL) * 100;
	}
	return 0;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
	long long milliseconds = (long long)duration->tv_sec * 1000 + (duration->tv_nsec + 999999) / 1000000;

	Sleep(milliseconds > 0x7fffffff ? 0x7fffffff : (DWORD)milliseconds);
	if (remaining)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *time, struct timespec *remaining)
{
	struct timespec now;
	long long nanoseconds;

	if (!(flags & TIMER_ABSTIME))
		return nanosleep(time, remaining);
	/* sleep until about a millisecond before the deadline (Sleep rounds
	up to the timer period), then yield until it passes */
	for (;;)
	{
		clock_gettime(clock, &now);
		nanoseconds = ((long long)time->tv_sec - now.tv_sec) * 1000000000LL + (time->tv_nsec - now.tv_nsec);
		if (nanoseconds <= 0)
			return 0;
		if (nanoseconds > 2000000)
			Sleep((DWORD)(nanoseconds / 1000000 - 1));
		else
			SwitchToThread();
	}
}

int sched_yield(void)
{
	SwitchToThread();
	return 0;
}
