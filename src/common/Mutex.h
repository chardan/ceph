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

#ifndef CEPH_MUTEX_H
#define CEPH_MUTEX_H

#include <mutex>

#include <pthread.h>

#include "common/Clock.h"
#include "common/ceph_context.h"

#include "lockdep.h"

// clobber other asserts:
#include "include/assert.h"

using namespace ceph;

class PerfCounters;

namespace detail {

template <typename MutexT>
class Mutex_Common;

} // namespace detail

enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};

// Interface:
// - the unfortunate name is a consequence of needing to fit in with extant behavior
class Mutex
{
 template <typename MutexT>
 friend class detail::Mutex_Common;

 public:
 enum class lockdep_flag : bool { enable = true, disable = false };

 private:
 pthread_t	locked_by;

 protected:
 std::string name;

 bool lockdep 	= true;  // JFW: lockdep used only for non-recursive mutexes..?
 bool backtrace	= false;

 int id 	= 0;

 int nlocks	= 0;

 CephContext	*cct	 = nullptr;
 PerfCounters	*logger  = nullptr;

 private:
 Mutex(const Mutex&)		= delete;
 void operator=(const Mutex&)	= delete;

 public:
 Mutex(const std::string& name_);
 Mutex(const std::string& name_, lockdep_flag);	// JFW: occurs only once so far..
 Mutex(const std::string& name_, CephContext *cct_);

 virtual ~Mutex();

 public:
 bool is_locked() const       { return nlocks; }
 bool is_locked_by_me()	const { return is_locked() && pthread_self() == locked_by; }

 public:
 virtual void Lock(bool no_lockdep) = 0;

 virtual void Lock() 	= 0;

 virtual void Unlock()	= 0;

 virtual bool TryLock()	= 0;

 // Model BasicLockable:
 void lock()		{ return Lock(); }
 void unlock()		{ return Unlock(); }
 bool try_lock()	{ return TryLock(); }

 // Ceph interface:
 using Locker = std::lock_guard<Mutex>;
};

namespace detail {

template <typename MutexT>
struct Mutex_Common
{
 friend class Cond;

 public:
 MutexT mtx; 

 public:
 ~Mutex_Common()
 {
#warning JFW this might require more fiddling... hopefully NOT up to requiring two completely impls
/* JFW: 
     // helgrind gets confused by condition wakeups leading to mutex destruction
     ANNOTATE_BENIGN_RACE_SIZED(&mtx, sizeof(mtx), "Mutex primitive");
*/
 }

 public:

 template <typename OwnerT>
 void lock_self(OwnerT& owner)
 {
  // instrumented mutex enabled (JFW: not sure why if(logger) is insufficient):
  if (owner.logger && owner.cct && owner.cct->_conf->mutex_perf_counter) {
    utime_t start;

    start = ceph_clock_now();

    // Already locked:
    if(owner.TryLock()) 
     return;

    mtx.lock();

    owner.logger->tinc(l_mutex_wait,
		       ceph_clock_now() - start);
  } else {
    mtx.lock();
  }

  owner.nlocks++;

  owner.locked_by = pthread_self();
 }

 template <typename OwnerT>
 void unlock_self(OwnerT& owner)
 {
  assert(pthread_self() == owner.locked_by);

  mtx.unlock(); 
 
  owner.nlocks--;
  owner.locked_by = 0;
 }

 template <typename OwnerT>
 bool try_lock_self(OwnerT& owner)
 {
  // We're already locked:
  if(mtx.try_lock())
   return true;

  // Note that if logging is on, this calls try_lock() again.
  lock_self(owner);

  return false;
 }

};

} // namespace detail

class BasicMutex : public Mutex, 
                   public detail::Mutex_Common<std::mutex>
{
 public:
 BasicMutex(const std::string& name_)
  : Mutex(name_)
 {}

 BasicMutex(const std::string& name_, CephContext *cct_)
  : Mutex(name_, cct_)
 {}

 BasicMutex(const std::string& name_, lockdep_flag ldf)
  : Mutex(name_, ldf)
 {}
 
 public:
 void Lock(bool no_lockdep) override;
 void Lock() override { return Lock(false); }
 void Unlock() override;

 bool TryLock() override;
};

class RecursiveMutex : public Mutex,
                       public detail::Mutex_Common<std::recursive_mutex>
{
 public:
 RecursiveMutex(const std::string& name_)
  : Mutex(name_)
 {}

 RecursiveMutex(const std::string& name_, CephContext *cct_)
  : Mutex(name_, cct_)
 {}

 RecursiveMutex(const std::string& name_, lockdep_flag ldf)
  : Mutex(name_, ldf)
 {}

 public:
 void Lock(bool no_lockdep) override;
 void Lock() override { return Lock(false); }
 void Unlock() override;

 bool TryLock() override;
};

/* JFW:
class OLD_Mutex {
private:
  std::string name;
  int id;
  bool recursive;
  bool lockdep;
  bool backtrace;  // gather backtrace on lock acquisition

  pthread_mutex_t _m;
  int nlock;
  pthread_t locked_by;
  CephContext *cct;
  PerfCounters *logger;

  // don't allow copying.
  void operator=(const Mutex &M);
  Mutex(const Mutex &M);

  void _register() {
    id = lockdep_register(name.c_str());
  }
  void _will_lock() { // about to lock
    id = lockdep_will_lock(name.c_str(), id, backtrace);
  }
  void _locked() {    // just locked
    id = lockdep_locked(name.c_str(), id, backtrace);
  }
  void _will_unlock() {  // about to unlock
    id = lockdep_will_unlock(name.c_str(), id);
  }

public:
  Mutex(const std::string &n, bool r = false, bool ld=true, bool bt=false,
	CephContext *cct = 0);
  ~Mutex();
  bool is_locked() const {
    return (nlock > 0);
  }
  bool is_locked_by_me() const {
    return nlock > 0 && locked_by == pthread_self();
  }

  bool TryLock() {
    int r = pthread_mutex_trylock(&_m);
    if (r == 0) {
      if (lockdep && g_lockdep) _locked();
      _void Mutex_Common::lock_self()
{
  // instrumented mutex enabled (JFW: not sure why if(logger) is insufficient):
  if (logger && cct && cct->_conf->mutex_perf_counter) {
    utime_t start;

    start = ceph_clock_now();

    if(TryLock()) 
     return;

    mtx.lock();

    logger->tinc(l_mutex_wait,
		 ceph_clock_now() - start);
  } else {
    mtx.lock();
  }

  nlock++;

  locked_by = pthread_self();
}

void Mutex_Common::unlock_self()
{
 assert(pthread_self() == locked_by);

 mtx.unlock();

 nlock--;
 locked_by = 0;
}

bool Mutex_Common::try_lock_self()
{
  // We're already locked:
  if(mtx.try_lock())
   return true;

  // Lock acquired:
  nlock++;

  return false;
}

post_lock();
    }
    return r == 0;
  }

  void Lock(bool no_lockdep=false);

  void _post_lock() {
    if (!recursive) {
      assert(nlock == 0);
      locked_by = pthread_self();
    };
    nlock++;
  }

  void _pre_unlock() {
    assert(nlock > 0);
    --nlock;
    if (!recursive) {
      assert(locked_by == pthread_self());
      locked_by = 0;
      assert(nlock == 0);
    }
  }
  void Unlock();

  friend class Cond;


public:
  class Locker {
    Mutex &mutex;

  public:
    explicit Locker(Mutex& m) : mutex(m) {
      mutex.Lock();
    }
    ~Locker() {
      mutex.Unlock();
    }
  };
};
*/

#endif
