/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#pragma once

#include "SC_Lock.h"

// On Linux (non-Cobalt), back SC_SyncCondition with a POSIX sem_t. The previous
// condition_variable_any implementation is RT-hostile: libstdc++ implements
// condition_variable_any::notify_one() by taking an internal mutex (it has to,
// in order to synchronize with waits on arbitrary Lockable types), which means
// Signal() — called once per audio buffer from the RT thread — can block on
// the waiter thread. sem_post is a plain futex wake with no internal locking
// and is async-signal-safe. Other platforms keep the cv-based implementation.
#if defined(__linux__) && !defined(__COBALT__)
#    define SC_SYNC_CONDITION_USE_POSIX_SEMAPHORE 1
#    include <cerrno>
#    include <semaphore.h>
#else
#    define SC_SYNC_CONDITION_USE_POSIX_SEMAPHORE 0
#endif

class SC_SyncCondition {
public:
#if SC_SYNC_CONDITION_USE_POSIX_SEMAPHORE

    SC_SyncCondition() { sem_init(&mSem, 0, 0); }
    ~SC_SyncCondition() { sem_destroy(&mSem); }

    SC_SyncCondition(const SC_SyncCondition&) = delete;
    SC_SyncCondition& operator=(const SC_SyncCondition&) = delete;

    // Process exactly one pending signal, blocking if none.
    void WaitEach() {
        while (sem_wait(&mSem) != 0 && errno == EINTR) { }
    }

    // Drain every signal that arrived before entry, blocking only if there
    // were none. Matches the "writeSnapshot" semantics of the old cv-based
    // implementation closely enough for the single call site (DiskIO uses
    // WaitEach, scsynth drivers use WaitNext — WaitOnce is unused today but
    // kept for interface compatibility).
    void WaitOnce() {
        if (sem_trywait(&mSem) == 0) {
            while (sem_trywait(&mSem) == 0) { }
            return;
        }
        while (sem_wait(&mSem) != 0 && errno == EINTR) { }
        while (sem_trywait(&mSem) == 0) { }
    }

    // Discard any already-pending signals, then block for a fresh one. This
    // is what SC_AudioDriver::RunThread uses: it never cares about missed
    // wakeups, only that it wakes up again once more work arrives.
    void WaitNext() {
        while (sem_trywait(&mSem) == 0) { }
        while (sem_wait(&mSem) != 0 && errno == EINTR) { }
    }

    void Signal() { sem_post(&mSem); }

private:
    sem_t mSem;

#else // SC_SYNC_CONDITION_USE_POSIX_SEMAPHORE — original cv-based implementation

    SC_SyncCondition(): read(0), write(0) {}

    ~SC_SyncCondition() {}

    void WaitEach() {
        // waits if it has caught up.
        // not very friendly, may be trying in vain to keep up.
        unique_lock<SC_Lock> lock(mutex);
        while (read == write)
            available.wait(lock);
        ++read;
    }

    void WaitOnce() {
        // waits if not signaled since last time.
        // if only a little late then can still go.

        unique_lock<SC_Lock> lock(mutex);
        int writeSnapshot = write;
        while (read == writeSnapshot)
            available.wait(lock);
        read = writeSnapshot;
    }

    void WaitNext() {
        // will wait for the next signal after the read = write statement
        // this is the friendliest to other tasks, because if it is
        // late upon entry, then it has to lose a turn.
        unique_lock<SC_Lock> lock(mutex);
        read = write;
        while (read == write)
            available.wait(lock);
    }

    void Signal() {
        ++write;
#    ifdef SC_CONDITION_VARIABLE_ANY_SHOULD_LOCK_BEFORE_NOTIFY
        if (mutex.try_lock()) {
            available.notify_one();
            mutex.unlock();
        }
#    else // CONDITION_VARIABLE_ANY_SHOULD_LOCK_BEFORE_NOTIFY
        available.notify_one();
#    endif // CONDITION_VARIABLE_ANY_SHOULD_LOCK_BEFORE_NOTIFY
    }

private:
    // the mutex is only for pthread_cond_wait, which requires it.
    // since there is only supposed to be one signaller and one waiter
    // there is nothing to mutually exclude.
    condition_variable_any available;
    SC_Lock mutex;
    int read, write;

#endif // SC_SYNC_CONDITION_USE_POSIX_SEMAPHORE
};
