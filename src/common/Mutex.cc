// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/Mutex.h"
#include "common/config.h"
#include "common/valgrind.h"
#include "common/perf_counters.h"

Mutex::Mutex(const std::string& name_, CephContext *cct_)
 : name(name_),
   cct(cct_)
{
 ANNOTATE_BENIGN_RACE_SIZED(&id, sizeof(id), "Mutex lockdep id");
 ANNOTATE_BENIGN_RACE_SIZED(&nlocks, sizeof(nlocks), "Mutex nlock");
 ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "Mutex locked_by");

// JFW: this applies only if NOT recursive..?
 if(lockdep)
  { 
/*
 // JFW: I'm not sure there's a standard library equivalent...
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&_m, &attr);
    pthread_mutexattr_destroy(&attr);
*/
  }

 if(g_lockdep)
  id = lockdep_register(name.c_str());

 if(nullptr != cct) {
   PerfCountersBuilder b(cct, string("mutex-") + name, l_mutex_first, l_mutex_last);
   b.add_time_avg(l_mutex_wait, "wait", "Average time of mutex in locked state");
   logger = b.create_perf_counters();
   cct->get_perfcounters_collection()->add(logger);
   logger->set(l_mutex_wait, 0);
 }
}

Mutex::Mutex(const std::string& name_, lockdep_flag ld_flag)
 : Mutex(name_, nullptr)
{
 assert(lockdep_flag::disable == ld_flag);
 // JFW: I've seen this happen only once in the code so far...
 lockdep = static_cast<bool>(ld_flag); 
}

Mutex::Mutex(const std::string& name_)
 : Mutex(name_, nullptr)
{}

Mutex::~Mutex()
{
  assert(nlocks == 0);

  if (cct && logger) {
    cct->get_perfcounters_collection()->remove(logger);
#warning JFW move me to a smart pointer
    delete logger;
  }

  if (lockdep && g_lockdep) {
    lockdep_unregister(id);
  }
}

void BasicMutex::Lock(bool no_lockdep)
{
 if(lockdep && g_lockdep && !no_lockdep) 
  id = lockdep_will_lock(name.c_str(), id, backtrace);

 lock_self(*this);

 if(lockdep && g_lockdep) 
  id = lockdep_locked(name.c_str(), id, backtrace);
}

void BasicMutex::Unlock()
{
 if(lockdep && g_lockdep)
  id = lockdep_will_unlock(name.c_str(), id);

 unlock_self(*this);

 assert(0 == nlocks);
}

bool BasicMutex::TryLock() 
{
 if(try_lock_self(*this))
  return true;

 if(lockdep && g_lockdep)
  id = lockdep_locked(name.c_str(), id, backtrace);

 return false;
}

void RecursiveMutex::Lock(bool no_lockdep)
{
 // Should never be true on a recursive mutex:
 assert(no_lockdep);

 lock_self(*this);
}

void RecursiveMutex::Unlock()
{
 unlock_self(*this);
}

bool RecursiveMutex::TryLock()
{
 return try_lock_self(*this);
}

