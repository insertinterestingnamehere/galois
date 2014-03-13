/** pthread thread pool implementation -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#include "Galois/Runtime/Sampling.h"
#include "Galois/Runtime/ThreadPool.h"
#include "Galois/Runtime/ll/EnvCheck.h"
#include "Galois/Runtime/ll/HWTopo.h"
#include "Galois/Runtime/ll/TID.h"
#include "Galois/Runtime/ll/gio.h"

#include "boost/utility.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cassert>

#include <semaphore.h>
#include <pthread.h>

// Forward declare this to avoid including PerThreadStorage.
// We avoid this to stress that the thread Pool MUST NOT depend on PTS.
namespace Galois {
namespace Runtime {
extern void initPTS();
}
}

using namespace Galois::Runtime;

namespace {

class Semaphore: private boost::noncopyable {
  sem_t sem;
public:
  explicit Semaphore(int val = 0) {
    if (sem_init(&sem, 0, val))
      GALOIS_DIE("PTHREAD");
  }

  ~Semaphore() {
    if (sem_destroy(&sem))
      GALOIS_DIE("PTHREAD");
  }

  void release(int n = 1) {
    while (n) {
      --n;
      if (sem_post(&sem))
        GALOIS_DIE("PTHREAD");
    }
  }

  void acquire(int n = 1) {
    while (n) {
      --n;
      int rc;
      while (((rc = sem_wait(&sem)) < 0) && (errno == EINTR)) { }
      if (rc)
        GALOIS_DIE("PTHREAD");
    }
  }
};

class ThinBarrier: private boost::noncopyable {
  volatile int started;
public:
  ThinBarrier(int v) { }
  void release(int n = 1) {
    __sync_fetch_and_add(&started, 1);
  }
  void acquire(int n = 1) {
    while (started < n) { }
  }
  void reset() { started = 0; }
};


class ThreadPool_pthread : public ThreadPool {
  pthread_t* threads; // Set of threads
  Semaphore* starts;  // Signal to release threads to run
  ThinBarrier started;
  volatile bool shutdown; // Set and start threads to have them exit
  volatile unsigned starting; // Each run call uses this to control num threads
  volatile RunCommand* workBegin; // Begin iterator for work commands
  volatile RunCommand* workEnd; // End iterator for work commands

  void initThread(unsigned tid) {
    // Initialize TID
    Galois::Runtime::LL::initTID(tid);
    Galois::Runtime::initPTS();
    if (!LL::EnvCheck("GALOIS_DO_NOT_BIND_THREADS"))
      if (tid != 0 || !LL::EnvCheck("GALOIS_DO_NOT_BIND_MAIN_THREAD"))
	Galois::Runtime::LL::bindThreadToProcessor(tid);
    // Use a simple pthread or atomic to avoid depending on Galois
    // too early in the initialization process
    started.release();
  }

  void cascade(int tid) {
    const unsigned multiple = 2;
    for (unsigned i = 1; i <= multiple; ++i) {
      unsigned n = tid * multiple + i;
      if (n < starting)
        starts[n].release();
    }
  }

  void doWork() {
    RunCommand* workPtr = (RunCommand*)workBegin;
    RunCommand* workEndL = (RunCommand*)workEnd;
    while (workPtr != workEndL) {
      (*workPtr)();
      ++workPtr;
    }
  }

  void prefixThreadWork(unsigned tid) {
    if (tid)
      Galois::Runtime::beginThreadSampling();
  }

  void suffixThreadWork(unsigned tid) {
    if (tid)
      Galois::Runtime::endThreadSampling();
  }

  void launch(unsigned tid) {
    while (!shutdown) {
      starts[tid].acquire();
      cascade(tid);
      prefixThreadWork(tid);
      doWork();
      suffixThreadWork(tid);
      started.release();
    }
  }

  static void* slaunch(void* V) {
    ThreadPool_pthread* TP = (ThreadPool_pthread*)V;
    static unsigned next = 0;
    unsigned tid = __sync_add_and_fetch(&next, 1);
    TP->initThread(tid);
    TP->launch(tid);
    return 0;
  }
  
public:
  ThreadPool_pthread():
    ThreadPool(Galois::Runtime::LL::getMaxThreads()),
    started(0), shutdown(false), workBegin(0), workEnd(0)
  {
    initThread(0);

    starts = new Semaphore[maxThreads];
    threads = new pthread_t[maxThreads];

    for (unsigned i = 1; i < maxThreads; ++i) {
      if (pthread_create(&threads[i], 0, &slaunch, this))
        GALOIS_DIE("PTHREAD");
    }
    started.acquire(maxThreads);
  }

  virtual ~ThreadPool_pthread() {
    shutdown = true;
    workBegin = workEnd = 0;
    __sync_synchronize();
    for (unsigned i = 1; i < maxThreads; ++i)
      starts[i].release();
    for (unsigned i = 1; i < maxThreads; ++i) {
      if (pthread_join(threads[i], NULL))
        GALOIS_DIE("PTHREAD");
    }
    delete [] starts;
    delete [] threads;
  }

  virtual void runInternal(unsigned num, RunCommand* begin, RunCommand* end) {
    // Sanitize num
    num = std::min(std::max(num,1U), maxThreads);
    starting = num;
    // Setup work
    workBegin = begin;
    workEnd = end;
    // Ensure stores happen before children are spawned
    __sync_synchronize();
    started.reset();
    cascade(0);
    // Do master thread work
    doWork();
    //wait for children
    started.acquire(num - 1);
    // Clean up
    workBegin = workEnd = 0;
  }
};

} // end namespace

//! Implement the global threadpool
ThreadPool& Galois::Runtime::getSystemThreadPool() {
  static ThreadPool_pthread pool;
  return pool;
}
