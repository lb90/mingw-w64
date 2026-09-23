/*
   Copyright (c) 2011-2016  mingw-w64 project

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* public header files */
#include "pthread.h"
/* internal header files */
#include "misc.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

void (WINAPI *_pthread_get_system_time_best_as_file_time) (LPFILETIME) = NULL;
static ULONGLONG (WINAPI *_pthread_get_tick_count_64) (VOID);
HRESULT (WINAPI *_pthread_set_thread_description) (HANDLE, PCWSTR) = NULL;
BOOL (WINAPI *_pthread_get_handle_information) (HANDLE, LPDWORD) = NULL;

pWaitOnAddress_t _pthread_wait_on_address;
pWakeByAddressSingle_t _pthread_wake_by_address_single;
pWakeByAddressAll_t _pthread_wake_by_address_all;

#if defined(__GNUC__) || defined(__clang__)
#if __GNUC__ >= 9 && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
#endif
__attribute__((constructor(0)))
#endif
static void winpthreads_init(void)
{
HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    HMODULE kernelbase = GetModuleHandleA("kernelbase.dll");

    assert (kernel32);

    _pthread_get_handle_information =
        (BOOL (WINAPI *)(HANDLE, LPDWORD))(void*) GetProcAddress(kernel32, "GetHandleInformation");

    _pthread_get_tick_count_64 =
        (ULONGLONG (WINAPI *)(VOID))(void*) GetProcAddress(kernel32, "GetTickCount64");

    _pthread_set_thread_description =
        (HRESULT (WINAPI *)(HANDLE, PCWSTR))(void*) GetProcAddress(kernel32, "SetThreadDescription");

    /* <1us precision on Windows 10 */
    _pthread_get_system_time_best_as_file_time =
        (void (WINAPI *)(LPFILETIME))(void*) GetProcAddress(kernel32, "GetSystemTimePreciseAsFileTime");

    if (!_pthread_get_system_time_best_as_file_time) {
        /* >15ms precision on Windows 10 */
        _pthread_get_system_time_best_as_file_time = GetSystemTimeAsFileTime;
    }

    if (kernelbase) {
        /* Although SetThreadDescription lives in kernel32.dll, on Windows Server 2016,
         * Windows 10 LTSB 2016 and Windows 10 version 1607, it was only available in
         * kernelbase.dll. So, load it from there for maximum coverage.
         */
        if (!_pthread_set_thread_description) {
            _pthread_set_thread_description =
                (HRESULT (WINAPI *)(HANDLE, PCWSTR))(void*) GetProcAddress(kernelbase, "SetThreadDescription");
        }

        _pthread_wait_on_address =
            (pWaitOnAddress_t) GetProcAddress(kernelbase, "WaitOnAddress");

        if (_pthread_wait_on_address) {
            _pthread_wake_by_address_single =
                (pWakeByAddressSingle_t) GetProcAddress(kernelbase, "WakeByAddressSingle");

            assert (_pthread_wake_by_address_single);

            _pthread_wake_by_address_all =
                (pWakeByAddressAll_t) GetProcAddress(kernelbase, "WakeByAddressAll");

            assert (_pthread_wake_by_address_all);
        }
    }
}
#if defined(__GNUC__) && __GNUC__ >= 9 && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/* Force a reference to __xc_t to prevent whole program optimization
 * from discarding the variable. */

/* On x86, symbols are prefixed with an underscore. */
# if defined(_M_IX86)
#   pragma comment(linker, "/include:___xc_t")
# else
#   pragma comment(linker, "/include:__xc_t")
# endif

#pragma section(".CRT$XCT", long, read)
__declspec(allocate(".CRT$XCT"))
extern const _PVFV __xc_t;
const _PVFV __xc_t = winpthreads_init;
#endif

unsigned long long _pthread_time_in_ms(void)
{
    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    return (((unsigned long long)ft.dwHighDateTime << 32) + ft.dwLowDateTime
            - 0x19DB1DED53E8000ULL) / 10000ULL;
}

unsigned long long _pthread_time_in_ms_from_timespec(const struct _timespec64 *ts)
{
    unsigned long long t = (unsigned long long) ts->tv_sec * 1000LL;
    /* The +999999 is here to ensure that the division always rounds up */
    t += (unsigned long long) (ts->tv_nsec + 999999) / 1000000;

    return t;
}

unsigned long long _pthread_rel_time_in_ms(const struct _timespec64 *ts)
{
    unsigned long long t1 = _pthread_time_in_ms_from_timespec(ts);
    unsigned long long t2 = _pthread_time_in_ms();

    /* Prevent underflow */
    if (t1 < t2) return 0;
    return t1 - t2;
}

static unsigned long long
_pthread_get_tick_count (long long *frequency)
{
  if (_pthread_get_tick_count_64 != NULL)
    return _pthread_get_tick_count_64 ();

  LARGE_INTEGER freq, timestamp;

  if (*frequency == 0)
  {
    if (QueryPerformanceFrequency (&freq))
      *frequency = freq.QuadPart;
    else
      *frequency = -1;
  }

  if (*frequency > 0 && QueryPerformanceCounter (&timestamp))
    return timestamp.QuadPart / (*frequency / 1000);

  /* Fallback */
  return GetTickCount ();
}

/* A wrapper around WaitForSingleObject() that ensures that
 * the wait function does not time out before the time
 * actually runs out. This is needed because WaitForSingleObject()
 * might have poor accuracy, returning earlier than expected.
 * On the other hand, returning a bit *later* than expected
 * is acceptable in a preemptive multitasking environment.
 */
unsigned long
_pthread_wait_for_single_object (void *handle, unsigned long timeout)
{
  DWORD result;
  unsigned long long start_time, end_time;
  unsigned long wait_time;
  long long frequency = 0;

  if (timeout == INFINITE || timeout == 0)
    return WaitForSingleObject ((HANDLE) handle, (DWORD) timeout);

  start_time = _pthread_get_tick_count (&frequency);
  end_time = start_time + timeout;
  wait_time = timeout;

  do
  {
    unsigned long long current_time;

    result = WaitForSingleObject ((HANDLE) handle, (DWORD) wait_time);
    if (result != WAIT_TIMEOUT)
      break;

    current_time = _pthread_get_tick_count (&frequency);
    if (current_time >= end_time)
      break;

    wait_time = (DWORD) (end_time - current_time);
  } while (TRUE);

  return result;
}

/* A wrapper around WaitForMultipleObjects() that ensures that
 * the wait function does not time out before the time
 * actually runs out. This is needed because WaitForMultipleObjects()
 * might have poor accuracy, returning earlier than expected.
 * On the other hand, returning a bit *later* than expected
 * is acceptable in a preemptive multitasking environment.
 */
unsigned long
_pthread_wait_for_multiple_objects (unsigned long count, void **handles, unsigned int all, unsigned long timeout)
{
  DWORD result;
  unsigned long long start_time, end_time;
  unsigned long wait_time;
  long long frequency = 0;

  if (timeout == INFINITE || timeout == 0)
    return WaitForMultipleObjects ((DWORD) count, (HANDLE *) handles, all, (DWORD) timeout);

  start_time = _pthread_get_tick_count (&frequency);
  end_time = start_time + timeout;
  wait_time = timeout;

  do
  {
    unsigned long long current_time;

    result = WaitForMultipleObjects ((DWORD) count, (HANDLE *) handles, all, (DWORD) wait_time);
    if (result != WAIT_TIMEOUT)
      break;

    current_time = _pthread_get_tick_count (&frequency);
    if (current_time >= end_time)
      break;

    wait_time = (DWORD) (end_time - current_time);
  } while (TRUE);

  return result;
}

#define TABLE_ALLOCATION_MINIMUM 8U
#define TABLE_ALLOCATION_MAXIMUM (UINT_MAX / sizeof (winpthreads_table_entry_t))

/**
 * winpthreads_table_initialize:
 *
 * Initializes a table that is dynamically allocated. Note that
 * zero-initialization works fine, so a default-initialized
 * winpthreads_table_t need not be initialized explicitly.
 */
void
winpthreads_table_initialize (winpthreads_table_t *table)
{
  memset (table, 0, sizeof (*table));
}

/**
 * winpthreads_table_finalize:
 *
 * Frees internal data associated with the table. Values are not
 * freed, that's responsibility of the user.
 */
void
winpthreads_table_finalize (winpthreads_table_t *table)
{
  free (table->entries);

  memset (table, 0, sizeof (*table));
}

static int
table_try_expand (winpthreads_table_t *table)
{
#if defined (__STD_C_VERSION__) && __STD_C_VERSION__ >= 201112L
  _Static_assert (SIZE_MAX >= UINT_MAX, "");
#endif

  if (table->entries_allocated > TABLE_ALLOCATION_MAXIMUM / 2)
    return FALSE;

  unsigned int new_entries_allocated = MAX (TABLE_ALLOCATION_MINIMUM, table->entries_allocated * 2);
  winpthreads_table_entry_t *new_entries = realloc (table->entries, new_entries_allocated * sizeof (winpthreads_table_entry_t));

  if (!new_entries)
    return FALSE;

  memset (new_entries + table->entries_allocated, 0,
          (new_entries_allocated - table->entries_allocated) * sizeof (winpthreads_table_entry_t));

  table->entries = new_entries;
  table->entries_allocated = new_entries_allocated;

  return TRUE;
}

static void
table_maybe_shrink (winpthreads_table_t *table)
{
  if (table->entries_count <= table->entries_allocated / 4 &&
      table->entries_allocated / 2 >= TABLE_ALLOCATION_MINIMUM)
  {
    /* Move busy entries on the right-half into free
     * entries on the left-half
     */
    unsigned int index_right = table->entries_allocated - 1;
    unsigned int index_left = 0;

    for (; index_right >= table->entries_allocated / 2; index_right--) {
      if (table->entries[index_right].key != NULL)
      {
        for (;; index_left++) {
          if (table->entries[index_left].key == NULL)
          {
            memcpy (&table->entries[index_left],
                    &table->entries[index_right],
                    sizeof (table->entries[0]));
            index_left++;
            break;
          }
        }

        assert (index_left < table->entries_allocated);
      }
    }

    unsigned int new_entries_allocated = table->entries_allocated / 2;
    winpthreads_table_entry_t *new_entries = realloc (table->entries, new_entries_allocated * sizeof (winpthreads_table_entry_t));

    if (new_entries == NULL)
      return;

    table->entries_allocated = new_entries_allocated;
    table->entries = new_entries;
  }
}

/**
 * winpthreads_table_find_entry:
 *
 * Finds an entry for the given key. Returns NULL if not present.
 * You can pass NULL to search for a free entry, but that's used
 * only by the implementation; users shouldn't call this function
 * wiht a NULL key.
 */
winpthreads_table_entry_t *
winpthreads_table_find_entry (winpthreads_table_t *table,
                              void                *key)
{
  for (unsigned int index = 0; index < table->entries_allocated; index++) {
    if (table->entries[index].key == key)
      return &table->entries[index];
  }

  return NULL;
}

/**
 * winpthreads_table_add_entry:
 *
 * Add a new entry for the given key, expanding the table if needed.
 * Note that there are a few constraints:
 *
 *  - key must not be NULL
 *  - key must not be present already in the table.
 *
 * This function is failable. If memory cannot be reallocated, the
 * return value is NULL.
 */
winpthreads_table_entry_t *
winpthreads_table_add_entry (winpthreads_table_t *table,
                             void                *key)
{
  winpthreads_table_entry_t *entry = NULL;

  assert (table->entries_count <= table->entries_allocated);

  if (table->entries_count < table->entries_allocated || table_try_expand (table)) {
    assert (table->entries_count < table->entries_allocated);

    /* Find a free entry (NULL key) */
    entry = winpthreads_table_find_entry (table, NULL);
    entry->key = key;

    table->entries_count++;
  }

  return entry;
}


/**
 * winpthreads_table_remove_entry:
 *
 * Remove an entry from the table. It's an error to pass an entry
 * that belongs to a different table.
 */
void
winpthreads_table_remove_entry (winpthreads_table_t       *table,
                                winpthreads_table_entry_t *entry)
{
  assert ((uintptr_t)entry >= (uintptr_t)&table->entries[0] &&
          (uintptr_t)entry < (uintptr_t)&table->entries[table->entries_allocated]);

  memset (entry, 0, sizeof (*entry));

  assert (table->entries_count > 0);
  table->entries_count--;

  table_maybe_shrink (table);
}
