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
#include "common/perf_counters.h"
#include "common/config.h"
#include "common/Clock.h"
#include "common/valgrind.h"

BasicMutex::BasicMutex(const std::string& name_, CephContext *cct_)
 : name(name_),
   cct(cct_)
{
#warning JFW fixup
// JFW: curious as to why it's ok that races on these are all right with us?
  ANNOTATE_BENIGN_RACE_SIZED(&lockdep_id, sizeof(lockdep_id), "Mutex lockdep id");
  ANNOTATE_BENIGN_RACE_SIZED(&nlocks, sizeof(nlocks), "Mutex nlock");
  ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "Mutex locked_by");

  if (cct) {
    PerfCountersBuilder b(cct, string("mutex-") + name, l_mutex_first, l_mutex_last);

    b.add_time_avg(l_mutex_wait, "wait", "Average time of mutex in locked state");

    assert(nullptr == logger);
    logger = b.create_perf_counters();
    assert(nullptr != logger);
    cct->get_perfcounters_collection()->add(logger);
    logger->set(l_mutex_wait, 0);
  }

/* JFW: this is the original Mutex behavior for lockdep == true (implicit in this class),
I don't know what really expresses this in std::mutex... 
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&_m, &attr);
    pthread_mutexattr_destroy(&attr);
*/
    if (g_lockdep)
     _register();
}

BasicMutex::~BasicMutex()
{
  assert(nlocks == 0);

  // helgrind gets confused by condition wakeups leading to mutex destruction
  ANNOTATE_BENIGN_RACE_SIZED(&mtx, sizeof(mtx), "Mutex primitive");

  if (cct && logger) 
   cct->get_perfcounters_collection()->remove(logger);

  if (g_lockdep) 
   lockdep_unregister(lockdep_id);
}

void BasicMutex::Lock()
{
 return Lock(lockdep_flag::enable);
}

void BasicMutex::Lock(const lockdep_flag lockdep)
{
  if (static_cast<bool>(lockdep) && g_lockdep) 
   _will_lock();

  if (logger && cct && cct->_conf->mutex_perf_counter) {
    utime_t start;
    // instrumented mutex enabled
    start = ceph_clock_now();
    if (TryLock()) {
	return;
    }

    mtx.lock();
 
    _post_lock();

    logger->tinc(l_mutex_wait,
		 ceph_clock_now() - start);
  } else { 
    mtx.lock();
  }

  if (static_cast<bool>(lockdep) && g_lockdep) 
   _locked();

  _post_lock();
}

bool BasicMutex::TryLock()
{
 if (false == mtx.try_lock())
  return false;

 if (g_lockdep)
  _locked();
 
 _post_lock();

 return true;
}

void BasicMutex::Unlock()
{
  _pre_unlock();

  if (g_lockdep) 
   _will_unlock();

  mtx.unlock();
}

NoLockDepMutex::NoLockDepMutex(const std::string& name_, CephContext *cct_)
 : name(name_),
   cct(cct_)
{
  ANNOTATE_BENIGN_RACE_SIZED(&nlocks, sizeof(nlocks), "Mutex nlock");
  ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "Mutex locked_by");

  if (cct) {
    PerfCountersBuilder b(cct, string("mutex-") + name, l_mutex_first, l_mutex_last);

    b.add_time_avg(l_mutex_wait, "wait", "Average time of mutex in locked state");

    assert(nullptr == logger);
    logger = b.create_perf_counters();
    assert(nullptr != logger);
    cct->get_perfcounters_collection()->add(logger);
    logger->set(l_mutex_wait, 0);
  }
}

NoLockDepMutex::~NoLockDepMutex()
{
  assert(nlocks == 0);

  // helgrind gets confused by condition wakeups leading to mutex destruction
  ANNOTATE_BENIGN_RACE_SIZED(&mtx, sizeof(mtx), "Mutex primitive");

  if (cct && logger) 
   cct->get_perfcounters_collection()->remove(logger);
}

void NoLockDepMutex::Lock()
{
  if (logger && cct && cct->_conf->mutex_perf_counter) {
    utime_t start;
    // instrumented mutex enabled
    start = ceph_clock_now();
    if (TryLock()) {
	return;
    }

    mtx.lock();

    logger->tinc(l_mutex_wait,
		 ceph_clock_now() - start);
  } else { 
    mtx.lock();
  }

  _post_lock();
}

bool NoLockDepMutex::TryLock()
{
 if (false == mtx.try_lock())
  return false;

 _post_lock();

 return true;
}

void NoLockDepMutex::Unlock()
{
  _pre_unlock();

  mtx.unlock();
}

RecursiveMutex::RecursiveMutex(const std::string& name_, CephContext *cct_)
 : name(name_),
   cct(cct_)
{
  ANNOTATE_BENIGN_RACE_SIZED(&lockdep_id, sizeof(lockdep_id), "Mutex lockdep id");
  ANNOTATE_BENIGN_RACE_SIZED(&nlocks, sizeof(nlocks), "Mutex nlock");

  if (cct) {
    PerfCountersBuilder b(cct, string("mutex-") + name,
			  l_mutex_first, l_mutex_last);
    b.add_time_avg(l_mutex_wait, "wait", "Average time of mutex in locked state");
    logger = b.create_perf_counters();
    cct->get_perfcounters_collection()->add(logger);
    logger->set(l_mutex_wait, 0);
  }

 if (g_lockdep)
  lockdep_id = lockdep_register(name.c_str());
}

RecursiveMutex::~RecursiveMutex() {

  assert(nlocks == 0);

  // helgrind gets confused by condition wakeups leading to mutex destruction
  ANNOTATE_BENIGN_RACE_SIZED(&mtx, sizeof(mtx), "Mutex primitive");

  if (cct && logger) {
    cct->get_perfcounters_collection()->remove(logger);
    delete logger;
  }
}

void RecursiveMutex::Lock()
{
 return Lock(lockdep_flag::disable);
}

void RecursiveMutex::Lock(const lockdep_flag lockdep)
{
  if (logger && cct && cct->_conf->mutex_perf_counter) {
    utime_t start;
    // instrumented mutex enabled
    start = ceph_clock_now();
    if (TryLock()) {
	return;
    }

    mtx.lock();

    logger->tinc(l_mutex_wait,
		 ceph_clock_now() - start);
  } else {
    mtx.lock();
  }

#warning JFW look into this
  // JFW: interestingly, this is the only situation where the lockdep flags
  // are checked /away/ from the recursion property. I can't tell if it's a
  // bug or not...:
  if (static_cast<bool>(lockdep) && g_lockdep)
   lockdep_id = lockdep_locked(name.c_str(), lockdep_id, backtrace);

  nlocks++;
}

void RecursiveMutex::Unlock() {
  if (g_lockdep) 
   lockdep_id = lockdep_will_unlock(name.c_str(), lockdep_id);

  mtx.unlock();

  nlocks--;
}

bool RecursiveMutex::TryLock() {
   
  if (false == mtx.try_lock())
   return false;

  nlocks++;

  return true;
}

