/*
   Copyright (c) 2026 mingw-w64 project

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

#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* public header files */
#include "pthread.h"

/* internal header files */
#include "misc.h"

/**
 * Reference:
 *
 * pthread_spin_init(), pthread_spin_destroy():
 *  <https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/functions/pthread_spin_destroy.html>
 *
 * pthread_spin_lock(), pthread_spin_trylock():
 *  <https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/functions/pthread_spin_lock.html>
 *
 * pthread_spin_unlock():
 *  <https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/functions/pthread_spin_unlock.html>
 *
 * Additional reads:
 *
 *  [1] LKML - "POSIX mutex destruction requirements vs. futexes"
 *      -> https://lkml.org/lkml/2014/11/27/472
 *
 */

/**
 * Lock state has 32 bits
 *
 *  MSB                                  LSB
 * +----------------------------------------+
 * | C | L |       LATE USE COUNT           |
 * +----------------------------------------+
 *
 * Legend:
 *
 *  Bit 31 : There are waiting threads (mutex is contended)
 *  Bit 30 : Mutex is locked
 *  Bits 29-0: Use count for the critical part of unlock
 *             (after state is set to unlocked but mutex
 *              still used, see [1]). This count is negative,
 *             i.e. it's 0 when all pthread_spin_unock have
 *             finished their work; -1 while there's an
 *             outstanding pthread_spin_unlock, and so on.
 */

#define BIT_CONTENDED 31U
#define BIT_LOCKED 30U

#define STATE_CONTENDED (1U << BIT_CONTENDED)
#define STATE_LOCKED (1U << BIT_LOCKED)

#define STATE_MASK (STATE_CONTENDED | STATE_LOCKED)
#define LATE_USE_COUNT_MASK (STATE_LOCKED - 1)

/**
 * Internal structure pointed to by `pthread_spinlock_t` objects.
 */
typedef struct {
  LONG State;
  /**
   * This value is used to indicate that spin lock has no owner.
   */
#define THREAD_ID_NO_OWNER ((DWORD)-1)
  /**
   * ID of the thread which holds the lock.
   */
  DWORD ThreadId;
  /**
   * Auto-reset event.
   *
   * When a thread releases the spinlock and `State` has `BIT_CONTENDED` set,
   * this event will be signaled in order to release one blocked thread.
   */
  HANDLE Event;
} WinpthreadsSpinlock;

#define WINPTHREADS_SPINLOCK_INITIALIZER ((WinpthreadsSpinlock *) PTHREAD_SPINLOCK_INITIALIZER)

/**
 * Obtain pointer to `WinpthreadsSpinlock` structure pointed to by `lock`.
 *
 * If `lock` points to statically initialized `pthread_spinlock_t` object,
 * allocate `WinpthreadsSpinlock` structure and store its address in `*lock`.
 *
 * On success, stores pointer to `WinpthreadsSpinlock` structure in `*spinlock`.
 *
 * Returns zero on success and an error-code on failure.
 */
static WINPTHREADS_INLINE int WinpthreadsSpinlockGet (pthread_spinlock_t *lock, WinpthreadsSpinlock **spinlock)
{
  WinpthreadsSpinlock **pSpinlock = (WinpthreadsSpinlock **) lock;

  /**
   * POSIX:
   *
   * If an implementation detects that the value specified by the lock argument
   * to pthread_spin_*() does not refer to an initialized spin lock object,
   * it is recommended that the function should fail and report an [EINVAL] error.
   */
  if (unlikely (pSpinlock == NULL)) {
    return EINVAL;
  }

  if (*pSpinlock == WINPTHREADS_SPINLOCK_INITIALIZER) {
    /**
     * We need to avoid race condition when more than one thread calls
     * `pthread_spin_[try]lock` on statically initialized `pthread_spinlock_t`
     * at the same time.
     *
     * Store newly initialized spinlock in `spinlock`, which points to local
     * variable supplied by the caller, and then store it in `*lock`.
     *
     * If some other thread was faster then us, destroy newly created spinlock
     * and use spinlock pointed to by `lock`.
     */
    int error_code = pthread_spin_init ((pthread_spinlock_t *) spinlock, PTHREAD_PROCESS_PRIVATE);

    if (error_code) {
      return error_code;
    }

    WinpthreadsSpinlock *oldLock = (WinpthreadsSpinlock *) InterlockedCompareExchangePointer (
      (void **) pSpinlock, *spinlock, WINPTHREADS_SPINLOCK_INITIALIZER
    );

    /**
     * Some other thread was faster than us.
     */
    if (unlikely (oldLock != WINPTHREADS_SPINLOCK_INITIALIZER)) {
      pthread_spin_destroy ((pthread_spinlock_t *) spinlock);
      *spinlock = oldLock;
    }
  } else {
    *spinlock = *pSpinlock;
  }

  if (unlikely (*spinlock == NULL)) {
    return EINVAL;
  }

  return 0;
}

int pthread_spin_init (pthread_spinlock_t *lock, int pshared)
{
  WinpthreadsSpinlock **pSpinlock = (WinpthreadsSpinlock **) lock;

  if (unlikely (pSpinlock == NULL)) {
    return EINVAL;
  }

  if (unlikely (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
    return EINVAL;
  }

  if (unlikely (pshared == PTHREAD_PROCESS_SHARED)) {
    return ENOSYS;
  }

  WinpthreadsSpinlock *wSpinlock = calloc (1, sizeof (WinpthreadsSpinlock));

  /**
   * The pthread_spin_init() function shall fail if:
   *
   * [ENOMEM]
   *  Insufficient memory exists to initialize the lock.
   */
  if (wSpinlock == NULL) {
    return ENOMEM;
  }

  wSpinlock->State    = 0;
  wSpinlock->ThreadId = THREAD_ID_NO_OWNER;
  wSpinlock->Event    = CreateEventW (NULL, FALSE, FALSE, NULL);

  /**
   * The pthread_spin_init() function shall fail if:
   *
   * [EAGAIN]
   *  The system lacks the necessary resources to initialize another spin lock.
   */
  if (wSpinlock->Event == NULL) {
    free (wSpinlock);
    return EAGAIN;
  }

  *pSpinlock = wSpinlock;

  return 0;
}

int pthread_spin_destroy (pthread_spinlock_t *lock)
{
  WinpthreadsSpinlock **pSpinlock = (WinpthreadsSpinlock **) lock;

  /**
   * POSIX:
   *
   * If an implementation detects that the value specified by the lock argument
   * to pthread_spin_destroy() does not refer to an initialized spin lock object,
   * it is recommended that the function should fail and report an [EINVAL] error.
   */
  if (unlikely (pSpinlock == NULL)) {
    return EINVAL;
  }

  /**
   * If `lock` points to statically initialized `pthread_spinlock_t` object,
   * this will immediately invalidate it, minimizing the window for
   * `pthread_spin_lock` and `pthread_spin_trylock` to attempt using it.
   */
  WinpthreadsSpinlock *wSpinlock = InterlockedCompareExchangePointer (
    (void **) pSpinlock, NULL, WINPTHREADS_SPINLOCK_INITIALIZER
  );

  if (unlikely (wSpinlock == NULL)) {
    return EINVAL;
  }

  if (unlikely (wSpinlock == WINPTHREADS_SPINLOCK_INITIALIZER)) {
    return 0;
  }

  /**
   * POSIX:
   *
   * If an implementation detects that the value specified by the lock argument
   * to pthread_spin_destroy() or pthread_spin_init() refers to a locked spin
   * lock object, or detects that the value specified by the lock argument to
   * pthread_spin_init() refers to an already initialized spin lock object,
   * it is recommended that the function should fail and report an [EBUSY] error.
   */
  if (unlikely (InterlockedOr (&wSpinlock->State, STATE_LOCKED) & STATE_LOCKED)) {
    return EBUSY;
  }

  /**
   * Invalidate `pthread_spinlock_t` object pointed by `lock`.
   */
  InterlockedExchangePointer ((void **) pSpinlock, NULL);

  /**
   * pthread_spin_destroy() can run concurrently with pthread_spin_unlock() [1].
   * Wait for all in-flight pthread_spin_unlock() invocations to finish working
   * on the mutex.
   */
  while ((ReadNoFence_ (&wSpinlock->State) & LATE_USE_COUNT_MASK) != 0)
    _pthread_wait_for_single_object (wSpinlock->Event, INFINITE);

  CloseHandle (wSpinlock->Event);
  free (wSpinlock);

  return 0;
}

int pthread_spin_lock (pthread_spinlock_t *lock)
{
  WinpthreadsSpinlock *wSpinlock = NULL;

  int error_code = WinpthreadsSpinlockGet (lock, &wSpinlock);

  if (error_code) {
    return error_code;
  }

  DWORD threadId = GetCurrentThreadId ();

  /**
   * The pthread_spin_lock() function may fail if:
   *
   * [EDEADLK]
   *  A deadlock condition was detected.
   */
  if (unlikely (wSpinlock->ThreadId == threadId)) {
    return EDEADLK;
  }

  /**
   * Try fast path.
   */
  if (!(InterlockedOr (&wSpinlock->State, STATE_LOCKED) & STATE_LOCKED)) {
    goto locked;
  }

  /**
   * Setting `BIT_CONTENDED` in `wSpinlock->State` causes
   * `pthread_spin_unlock` to signal `wSpinlock->Event`.
   */
  LONG oldLockState = InterlockedOr (&wSpinlock->State, STATE_LOCKED | STATE_CONTENDED);

  if (likely (oldLockState & STATE_LOCKED)) {
    do {
      switch (_pthread_wait_for_single_object (wSpinlock->Event, INFINITE)) {
        /**
         * `wSpinlock->Event` was signaled.
         *
         * There is a small chance that another thread grabs the lock faster
         * than we do; lock state is updated before wait handle is signaled.
         */
        case WAIT_OBJECT_0:
          break;
        default:
          return EINVAL;
      }
    } while (unlikely (InterlockedOr (&wSpinlock->State, STATE_LOCKED | STATE_CONTENDED) & STATE_LOCKED));
  }

locked:
  wSpinlock->ThreadId = threadId;

  return 0;
}

int pthread_spin_trylock (pthread_spinlock_t *lock)
{
  WinpthreadsSpinlock *wSpinlock = NULL;

  int error_code = WinpthreadsSpinlockGet (lock, &wSpinlock);

  if (error_code) {
    return error_code;
  }

  /**
   * The pthread_spin_trylock() function shall fail if:
   *
   * [EBUSY]
   *  A thread currently holds the lock.
   */
  if (InterlockedOr (&wSpinlock->State, STATE_LOCKED) & STATE_LOCKED) {
    return EBUSY;
  }

  wSpinlock->ThreadId = GetCurrentThreadId ();

  return 0;
}

int pthread_spin_unlock (pthread_spinlock_t *lock)
{
  WinpthreadsSpinlock *wSpinlock = NULL;

  int error_code = WinpthreadsSpinlockGet (lock, &wSpinlock);

  if (error_code) {
    return error_code;
  }

  DWORD threadId = GetCurrentThreadId ();

  /**
   * POSIX:
   *
   * If an implementation detects that the value specified by the lock argument
   * to pthread_spin_unlock() refers to a spin lock object for which the
   * current thread does not hold the lock, it is recommended that the function
   * should fail and report an [EPERM] error.
   */
  if (unlikely (wSpinlock->ThreadId != threadId)) {
    return EPERM;
  }

  wSpinlock->ThreadId = THREAD_ID_NO_OWNER;

  LONG State = ReadNoFence_ (&wSpinlock->State);
  if (!(State & STATE_CONTENDED)) {
    /**
     * Reset `BIT_LOCKED` and increment the late use count. This all can be
     * done with a single Interlocked addition. This might leave a spurious
     * `BIT_CONTENDED` (if set by another thread right before this call),
     * but that's not a problem.
     */
    State = InterlockedExchangeAdd (&wSpinlock->State, (STATE_CONTENDED + STATE_LOCKED + 1));
  }
  else {
    /**
     * Reset both `BIT_CONTENDED`, `BIT_LOCKED` and increment the late use
     * count. This all can be done with a single Interlocked addition.
     */
    State = InterlockedExchangeAdd (&wSpinlock->State, (STATE_LOCKED + 1));
  }

  int ret = 0;

  /**
   * If `wSpinlock->State` had `BIT_CONTENDED` set, then some other thread
   * is waiting for `wSpinlock->Event` to become signaled.
   */
  if (State & STATE_CONTENDED) {
    if (!SetEvent (wSpinlock->Event)) {
      ret = EINVAL;
    }
  }

  InterlockedDecrement (&wSpinlock->State);

  return ret;
}
