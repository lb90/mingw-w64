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
 * - Cancellation (sigh!)
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

#ifndef InterlockedCompareExchangeNoFence
#define InterlockedCompareExchangeNoFence InterlockedCompareExchange
#endif

#ifndef InterlockedDecrementNoFence
#define InterlockedDecrementNoFence InterlockedDecrement
#endif

#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
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

/* External-data once (for downlevel support)
 *
 * We associate external data to a pthread_once_t. External data
 * is created on-the-fly by the first non-primary thread, and contains a
 * Windows event to wait on. We also maintain a table mapping
 * pthread_once_t to its external data.
 * can contain something to wait on, for example a Windows event.
 **/

typedef struct {
  LONG secondary_threads_count;
  HANDLE completion_event;
} external_data_t;

static external_data_t *
external_data_new (void)
{
  HANDLE completion_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  if (!completion_event)
    return NULL;

  external_data_t *data = malloc (sizeof (external_data_t));
  if (!data) {
    CloseHandle (completion_event);
    return NULL;
  }

  data->completion_event = completion_event;
  data->secondary_threads_count = 0;

  return data;
}

static void
external_data_free (external_data_t *data)
{
  CloseHandle (data->completion_event);
  free (data);
}

typedef struct {
  pthread_once_t *once;
  external_data_t *external_data;
} table_entry_t;

static struct {
  table_entry_t *entries;
  unsigned int entries_count;
  unsigned int entries_allocated;
  CRITICAL_SECTION lock;
} table;

#define TABLE_ALLOCATION_MINIMUM 8U

static void
table_initialize (void)
{
  assert (table.entries_count == 0);

  InitializeCriticalSection (&table.lock);
}

static void
table_finalize (void)
{
  assert (table.entries_count == 0);

  free (table.entries);

  DeleteCriticalSection (&table.lock);
}

static int
table_internal_expand_unlocked (void)
{
#if defined (__STD_C_VERSION__) && __STD_C_VERSION__ >= 201112L
  _Static_assert (SIZE_MAX >= UINT_MAX, "");
#endif

  if (table.entries_allocated > UINT_MAX / 2U / sizeof (table_entry_t))
    return FALSE;

  unsigned int old_entries_allocated = table.entries_allocated;
  size_t old_size_allocated = old_entries_allocated *
                              sizeof (table_entry_t);
  table_entry_t *old_entries = table.entries;

  unsigned int new_entries_allocated = MAX (TABLE_ALLOCATION_MINIMUM, old_entries_allocated * 2U);
  size_t new_size_allocated = new_entries_allocated *
                              sizeof (table_entry_t);
  table_entry_t *new_entries = realloc (old_entries, new_size_allocated);

  if (!new_entries)
    return FALSE;

  /* Zero-out the part with undefined contents */
  memset (new_entries + old_entries_allocated, 0,
          new_size_allocated - old_size_allocated);

  table.entries = new_entries;
  table.entries_allocated = new_entries_allocated;

  return TRUE;
}

static void
table_internal_maybe_shrink_unlocked (void)
{
  if (table.entries_count <= table.entries_allocated / 4 &&
      table.entries_allocated / 2 >= TABLE_ALLOCATION_MINIMUM)
  {
    unsigned int index_a = 0;
    unsigned int index_b = table.entries_allocated - 1;

    for (; index_b >= table.entries_allocated / 2; index_b--) {
      if (table.entries[index_b].once != NULL) {
        for (; ; index_a++) {
          if (table.entries[index_a].once == NULL) {
            memcpy (&table.entries[index_a],
                    &table.entries[index_b],
                    sizeof (table.entries[0]));
            index_a++;
            break;
          }
        }
        assert (index_a < table.entries_allocated);
      }
    }

    unsigned int new_entries_allocated = table.entries_allocated / 2;
    size_t new_size_allocated = new_entries_allocated * sizeof (table_entry_t);
    table_entry_t *new_entries = realloc (table.entries, new_size_allocated);
    if (new_entries == NULL)
      return;

    table.entries_allocated = new_entries_allocated;
    table.entries = new_entries;
  }
}

static int
table_find_unlocked (pthread_once_t *once,
                     unsigned int   *out_index)
{
  for (unsigned int index = 0; index < table.entries_allocated; index++)
  {
    if (table.entries[index].once == once)
    {
      *out_index = index;
      return TRUE;
    }
  }

  return FALSE;
}

static int
table_append_unlocked (pthread_once_t *once,
                       unsigned int   *out_index)
{
  unsigned int index;
  int ret;

  assert (table.entries_count <= table.entries_allocated);

  if (table.entries_count == table.entries_allocated &&
      !table_internal_expand_unlocked ())
  {
    return FALSE;
  }

  assert (table.entries_count < table.entries_allocated);

  /* Find a free entry */
  ret = table_find_unlocked (NULL, &index);
  assert (ret);

  external_data_t *external_data = external_data_new ();
  if (!external_data) {
    /* Is it ok to keep the table enlarged? */
    return FALSE;
  }

  table.entries[index].once = once;
  table.entries[index].external_data = external_data;
  table.entries_count++;

  *out_index = index;

  return TRUE;
}

static void
table_remove_unlocked (pthread_once_t *once,
                       unsigned int    index_hint)
{
  unsigned int index;

  assert (table.entries_count > 0);

  if (index_hint < table.entries_count &&
      table.entries[index_hint].once == once)
  {
    index = index_hint;
  }
  else
  {
    int ret = table_find_unlocked (once, &index);
    assert (ret);
  }

  {
    table_entry_t *entry = &table.entries[index];
    external_data_free (entry->external_data);
    memset (entry, 0, sizeof (*entry));
  }

  table.entries_count--;

  table_internal_maybe_shrink_unlocked ();
}

static int
edo_secondary_thread_register (pthread_once_t   *once,
                               external_data_t **out_external_data,
                               unsigned int     *out_cookie)
{
  unsigned int index;

  EnterCriticalSection (&table.lock);

  if (ReadNoFence (once) == 2)
    goto not_registered;

  if (!table_find_unlocked (once, &index) &&
      !table_append_unlocked (once, &index))
    goto not_registered;

  table_entry_t *entry = &table.entries[index];

  assert (entry->external_data->secondary_threads_count < LONG_MAX);
  entry->external_data->secondary_threads_count++;

  LeaveCriticalSection (&table.lock);

  *out_external_data = entry->external_data;
  *out_cookie = index;

  return TRUE;

not_registered:
  LeaveCriticalSection (&table.lock);

  return FALSE;
}

static void
edo_secondary_thread_unregister (pthread_once_t  *once,
                                 external_data_t *external_data,
                                 unsigned int     cookie)
{
  LONG new_count = InterlockedDecrementNoFence (&external_data->secondary_threads_count);

  if (new_count == 0) {
    EnterCriticalSection (&table.lock);

    table_remove_unlocked (once, cookie);

    LeaveCriticalSection (&table.lock);
  }
}

static void
edo_primary_thread_completed (pthread_once_t *once)
{
  unsigned int index;
  HANDLE completion_event = NULL;

  EnterCriticalSection (&table.lock);

  assert (ReadNoFence (once) == 2);

  if (table_find_unlocked (once, &index)) {
    table_entry_t *entry = &table.entries[index];

    completion_event = entry->external_data->completion_event;
    assert (completion_event != NULL);
  }

  LeaveCriticalSection (&table.lock);

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
          func ();

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

        /* Synchronize with the publishing from the primary thread. */
        state = ReadAcquireWrapper (once);
        assert (state == 2);

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
          func ();

          /* Publish the final "initialization done" value. */
          WriteReleaseWrapper (once, 2);

          /* Wake all waiters. */
          edo_primary_thread_completed (once);

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
        unsigned int cookie;

        /* Attempt to register as secondary thread in the external
         * data. This way we get an event that signals when once
         * is completed.
         */
        if (edo_secondary_thread_register (once, &data, &cookie))
        {
          DWORD ret = WaitForSingleObject (data->completion_event, INFINITE);
          assert (ret == WAIT_OBJECT_0);

          /* Unregister so that the last thread can clean up.
           */
          edo_secondary_thread_unregister (once, data, cookie);
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

          /* Synchronize with the publishing from the primary thread.
           */
          state = ReadAcquireWrapper (once);
          assert (state == 2);
        }
      }
    }
  }

  return 0;
}

/* Called from DLL_PROCESS_ATTACH */
void
once_static_initialize (void)
{
  if (!pWaitOnAddress)
    table_initialize ();
}

/* Called from DLL_PROCESS_DETACH unless the process is terminating */
void
once_static_finalize (void)
{
  if (!pWaitOnAddress)
    table_finalize ();
}
