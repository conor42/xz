/* Atomic addition wrapper
 *
 * Copyright (c) 2018 Conor McCarthy
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 *
 */

#ifndef FL2_ATOMIC_H
#define FL2_ATOMIC_H

#include "mythread.h"


/* atomic add */

#if defined(MYTHREAD_ENABLED) && defined(_WIN32)


typedef LONG volatile FL2_atomic;
#define ATOMIC_INITIAL_VALUE -1
#define FL2_atomic_increment(n) InterlockedIncrement(&n)
#define FL2_atomic_add(n, a) InterlockedAdd(&n, a)
#define FL2_nonAtomic_increment(n) (++n)


#elif defined(MYTHREAD_ENABLED) && defined(__GNUC__)


typedef long FL2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define FL2_atomic_increment(n) __sync_fetch_and_add(&n, 1)
#define FL2_atomic_add(n, a) __sync_fetch_and_add(&n, a)
#define FL2_nonAtomic_increment(n) (n++)


#elif defined(MYTHREAD_ENABLED) && defined(HAVE_STDATOMIC_H) /* C11 */


#include <stdatomic.h>

typedef _Atomic long FL2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define FL2_atomic_increment(n) atomic_fetch_add(&n, 1)
#define FL2_atomic_add(n, a) atomic_fetch_add(&n, a)
#define FL2_nonAtomic_increment(n) (n++)


#else  /* No atomics */


#	ifdef MYTHREAD_ENABLED
#		error  No atomic operations available. Use --disable-threads to configure a single-threaded build.
#	endif

typedef long FL2_atomic;
#define ATOMIC_INITIAL_VALUE 0
#define FL2_atomic_increment(n) (n++)
#define FL2_atomic_add(n, a) (n += (a))
#define FL2_nonAtomic_increment(n) (n++)


#endif /* atomics */


#endif /* FL2_ATOMIC_H */
