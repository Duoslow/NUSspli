/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2020-2021 V10lator <v10lator@myway.de>                    *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             *
 ***************************************************************************/

#ifndef NUSSPLI_THREAD_H
#define NUSSPLI_THREAD_H

#include <wut-fixups.h>

#include <stdbool.h>
#include <stdint.h>

#include <coreinit/atomic.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>

#define DEFAULT_STACKSIZE		0xFFF8

#ifdef __cplusplus
	extern "C" {
#endif

/*
 * Default Wii U thread priority is 16.
 * SDL changes its audio thread priority one lower, so to 15.
 * We want the SDL audio thread to be at THREAD_PRIORITY_LOW,
 * so that has to be 15, too
 */
typedef enum
{
    THREAD_PRIORITY_HIGH = 13,
    THREAD_PRIORITY_MEDIUM = 14,
    THREAD_PRIORITY_LOW = 15,
} THREAD_PRIORITY;

#define spinlock volatile uint32_t

#define SPINLOCK_FREE	false
#define SPINLOCK_LOCKED	true

#ifdef NUSSPLI_DEBUG
#include <utils.h>
#define spinCreateLock(lock, locked)					\
{														\
	debugPrintf("Spinlock 0x%08X created!", &lock);		\
	lock = locked;							\
}
static inline bool spinTryLock(spinlock lock)
{
	bool ret = OSCompareAndSwapAtomic(&lock, SPINLOCK_FREE, SPINLOCK_LOCKED);
	if(!ret)
		debugPrintfUnlocked("spinTryLock: LOCKED: 0x%08X", &lock);
	return ret;
}
#else
#define spinCreateLock(lock, locked)	lock = locked
#define spinTryLock(lock)				OSCompareAndSwapAtomic(&lock, SPINLOCK_FREE, SPINLOCK_LOCKED)
#endif
#define spinIsLocked(lock)				(lock)
#define spinLock(lock)					while(!spinTryLock(lock)) {}
#define spinLockAsMutex(lock)			while(!spinTryLock(lock)) { OSSleepTicks(256); }
#define spinReleaseLock(lock)			lock = SPINLOCK_FREE

OSThread *startThread(const char *name, THREAD_PRIORITY priority, size_t stacksize, OSThreadEntryPointFn mainfunc, int argc, char *argv, OSThreadAttributes attribs);

#ifdef NUSSPLI_DEBUG
#define stopThread(thread, ret)																																\
{																																							\
	OSJoinThread(thread, ret);																																\
	debugPrintf("STACK: %s: 0x%08X/0x%08X", thread->name, OSCheckThreadStackUsage(thread), ((uint32_t)thread->stackStart) - ((uint32_t)thread->stackEnd));	\
	OSDetachThread(thread);																																	\
	MEMFreeToDefaultHeap(thread);																															\
}
#else
#define stopThread(thread, ret)																	\
{																								\
	OSJoinThread(thread, ret);																	\
	OSDetachThread(thread);																		\
	MEMFreeToDefaultHeap(thread);																\
}
#endif

#ifdef __cplusplus
    }
#endif

#endif // NUSSPLI_THREAD_H
