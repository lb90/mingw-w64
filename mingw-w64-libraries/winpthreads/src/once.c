/*
 * Copyright (c) 2026 mingw-w64 project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Luca Bacci <luca.bacci@outlook.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <windows.h>

#include <assert.h>

#include "pthread.h"
#include "misc.h"

/* FIXME:
 * - We're creating a Windows event while holding the lock
 * - Implement a pool of Windows events
 * - Can we store the table index in the once?
 * - Cancellation of secondary threads
 * - Proper testing?
 * - Patch ldar at runtime to get ldapr?
 */

/* ReadAcquire / WriteRelease (defined in winnt.h) use a freestanding
 * memory barrier and not ldar / ldapr / stlr. For better performance
 * we use a custom implementation.
 */

#ifndef _MSC_VER

static LONG
ReadAcquire (const volatile LONG *ptr)
{
  return __atomic_load_n (ptr, __ATOMIC_ACQUIRE);
}

static VOID
WriteRelease (volatile LONG *ptr, LONG value)
{
  __atomic_store_n (ptr, value, __ATOMIC_RELEASE);
}

static LONG
ReadNoFence (const volatile LONG *ptr)
{
  return __atomic_load_n (ptr, __ATOMIC_RELAXED);
}

static VOID
WriteNoFence (volatile LONG *ptr, LONG value)
{
  __atomic_store_n (ptr, value, __ATOMIC_RELAXED);
}

#ifndef InterlockedCompareExchangeNoFence
#define InterlockedCompareExchangeNoFence InterlockedCompareExchange
#endif

#ifndef InterlockedDecrementNoFence
#define InterlockedDecrementNoFence InterlockedDecrement
#endif

#endif

/* FIXME: also on GNUC? */
static WINPTHREADS_ALWAYS_INLINE LONG
ReadAcquireWrapper (const LONG *ptr)
{
#if defined (_MSC_VER) && defined (_M_ARM64)
  LONG result =
# if _MSC_FULL_VER >= 193632407
    /* Newer MSVC has __load_acquire32 intrinsic */
    __load_acquire32 (ptr);
# elif defined (__ARM_ARCH) && __ARM_ARCH >= (8 * 100 + 3)
    /* Older MSVC, compiling for ARMv8.3 or later */
    __ldapr32 (ptr);
# else
    __ldar32 (ptr);
# endif
  _ReadWriteBarrier ();
  return result;
#else
  return ReadAcquire (ptr);
#endif
}

static WINPTHREADS_ALWAYS_INLINE void
WriteReleaseWrapper (LONG *ptr,
                     LONG  value)
{
#if defined (_MSC_VER) && defined (_M_ARM64)
  _ReadWriteBarrier ();
  __stlr32 (ptr, value);
#else
  WriteRelease (ptr, value);
#endif
}

/* Once with external data (OWED) for downlevel support
 *
 * We associate external data to a pthread_once_t. External data
 * is created on-the-fly by secondary threads and contains a
 * Windows event to wait on. This is pretty much an emulation of
 * WaitOnAddress / WakeByAddressAll.
 */

typedef struct {
  LONG waiters_count;
  HANDLE completion_event;
} external_data_t;

static external_data_t *
external_data_new (void)
{
  external_data_t *data;
  HANDLE completion_event;

  completion_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  if (!completion_event)
    return NULL;

  data = malloc (sizeof (external_data_t));
  if (!data) {
    CloseHandle (completion_event);
    return NULL;
  }

  data->waiters_count = 0;
  data->completion_event = completion_event;

  return data;
}

static void
external_data_free (external_data_t *data)
{
  CloseHandle (data->completion_event);

  free (data);
}

static struct {
  winpthreads_table_t table;
  CRITICAL_SECTION lock;
} downlevel;

static external_data_t *
downlevel_secondary_thread_register (pthread_once_t *once)
{
  winpthreads_table_entry_t *table_entry;
  external_data_t *data = NULL;

  EnterCriticalSection (&downlevel.lock);

  if (ReadNoFence (once) == 2) {
    /* Initialization already completed, nothing to do */
    goto leave;
  }

  table_entry = winpthreads_table_find_entry (&downlevel.table, once);
  if (!table_entry) {
    data = external_data_new ();
    if (!data)
      goto leave;

    table_entry = winpthreads_table_add_entry (&downlevel.table, once);
    if (!table_entry) {
      external_data_free (data);
      data = NULL;
      goto leave;
    }

    table_entry->value = data;
  }

  data = table_entry->value;
  if (data->waiters_count == LONG_MAX)
    goto leave;

  data->waiters_count++;

leave:
  LeaveCriticalSection (&downlevel.lock);

  return data;
}

static void
downlevel_secondary_thread_unregister (external_data_t *data)
{
  if (data) {
    if (InterlockedDecrementNoFence (&data->waiters_count) == 0)
      external_data_free (data);
  }
}

static void
downlevel_primary_thread_done (pthread_once_t *once)
{
  winpthreads_table_entry_t *table_entry;
  HANDLE completion_event = NULL;

  EnterCriticalSection (&downlevel.lock);

  table_entry = winpthreads_table_find_entry (&downlevel.table, once);
  if (table_entry) {
    external_data_t *data = table_entry->value;

    completion_event = data->completion_event;
    assert (completion_event != NULL);

    winpthreads_table_remove_entry (&downlevel.table, table_entry);
  }

  LeaveCriticalSection (&downlevel.lock);

  /* It's best to set the event out of the lock. This way
   * the awaken thread has a higher chance of entering the
   * lock.
   */
  if (completion_event)
  {
    DWORD ret = SetEvent (completion_event);
    assert (ret != 0);
  }
}

static void
downlevel_primary_thread_completed (pthread_once_t *once)
{
  WriteRelease (once, 2);

  downlevel_primary_thread_done (once);
}

static void
downlevel_primary_thread_cancelled (pthread_once_t *once)
{
  WriteNoFence (once, 0);

  downlevel_primary_thread_done (once);
}

static void
downlevel_once_cancelled (void *user_data)
{
  downlevel_primary_thread_cancelled ((pthread_once_t *) user_data);
}

static void
modern_once_cancelled (void *user_data)
{
  pthread_once_t *once = (pthread_once_t *) user_data;

  WriteNoFence (once, 0);

  pWakeByAddressSingle (once);
}

int
pthread_once (pthread_once_t *once,
              void (* func) (void))
{
  LONG state;

  /* POSIX imposes some error checking on the arguments */
  if (once == NULL)
    return EINVAL;

  if (func == NULL)
    return EINVAL;

  while (1)
  {
    state = ReadAcquireWrapper (once);
    if (state == 2) {
      /* Already initialized. */
      return 0;
    }

    if (pWaitOnAddress)
    {
      /* Implementation for modern Windows (Windows 8 and above) */

      switch (state) {
        case 0:
          /* Try to become the initializer thread. */
          if (InterlockedCompareExchangeNoFence (once, 1, 0) == 0) {
            /* Perform initialization */
            pthread_cleanup_push (modern_once_cancelled, once);
            func ();
            pthread_cleanup_pop (0);

            /* Publish the final "initialization done" value. */
            WriteReleaseWrapper (once, 2);

            /* Wake all waiters. */
            pWakeByAddressAll (once);

            return 0;
          }

          /* If we are here, another thread arrived first and
           * was granted with the initialization task.
           */

          /* fallthrough */
#ifdef __GNUC__
         __attribute__ ((fallthrough));
#endif
        case 1:
          while ((state = ReadNoFence (once)) == 1)
            pWaitOnAddress (once, &state, sizeof (state), INFINITE);
          break;
        default:
          abort ();
          break;
      }
    }
    else
    {
      /* External-data once (EDO) for downlevel support (up to Windows 7) */

      switch (state) {
        case 0:
          /* Attempt to become the primary thread. */
          if (InterlockedCompareExchangeNoFence (once, 1, 0) == 0) {
            /* Perform initialization. */
            pthread_cleanup_push (downlevel_once_cancelled, once);
            func ();
            pthread_cleanup_pop (0);

            /* Publish the final "initialization done" value
             * and wake all waiters.
             */
            downlevel_primary_thread_completed (once);

            return 0;
          }

          /* Another thread arrived first and was granted with the
           * initialization task. Now go waiting.
           */

          /* fallthrough */
#ifdef __GNUC__
         __attribute__ ((fallthrough));
#endif
        case 1:
        {
          external_data_t *data;

          /* Attempt to register as secondary thread in the external
           * data. This way we get an event that signals when once
           * is completed.
           */
          data = downlevel_secondary_thread_register (once);

          if (data) {
            DWORD ret = WaitForSingleObject (data->completion_event, INFINITE);
            assert (ret == WAIT_OBJECT_0);

            /* Unregister so that the last thread can clean up.
             */
            downlevel_secondary_thread_unregister (data);
          }
          else
          {
            /* Either initialization is already completed or resources
             * couldn't be allocated. Fall back to checks at regular
             * intervals. In case initialization is already completed,
             * we won't sleep at all.
             */
            while (ReadNoFence (once) == 1)
              Sleep (10);
          }
        }
      }
    }
  }

  return 0;
}

/* Called from DLL_PROCESS_ATTACH
 */
void
once_static_initialize (void)
{
  if (!pWaitOnAddress) {
    /* Zero-initialization of downlevel.table is fine */
    InitializeCriticalSection (&downlevel.lock);
  }
}

/* Called from DLL_PROCESS_DETACH (unless the process is terminating)
 */
void
once_static_finalize (void)
{
  if (!pWaitOnAddress) {
    DeleteCriticalSection (&downlevel.lock);
    winpthreads_table_finalize (&downlevel.table);
  }
}
