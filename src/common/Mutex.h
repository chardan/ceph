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
#include <thread>

#include "lockdep.h"

#include "common/ceph_context.h"

#include "include/assert.h"

using namespace ceph;

class PerfCounters;

enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};

class BasicMutex;
class NoLockdepBasicMutex;
class RecursiveMutex;

using Mutex = BasicMutex;

// BasicMutex: A non-recursive mutex with LockDep (the default):
// 	recursive = false, lockdep = true, backtrace = false
class BasicMutex 
{
 friend class Cond;

 public:
 enum class lockdep_flag : bool { enable = true, disable = false };

 // Ceph interface:
 using Locker = std::lock_guard<Mutex>;

 private:
 std::string name;

 int lockdep_id	= 0;

 std::mutex mtx;

 int nlocks = 0;

#warning JFW remove me
public:
 std::thread::id locked_by; 

 CephContext *cct      = nullptr;
 PerfCounters *logger  = nullptr;

#warning JFW need to provide a way to turn this on
 bool backtrace = false;

 private:
 BasicMutex(const BasicMutex&)   = delete;
 void operator=(const BasicMutex&) = delete;

 public:
 BasicMutex(const std::string& name_, CephContext *cct_);

 BasicMutex(const std::string& name_)
  : BasicMutex(name_, nullptr)
 {} 

 ~BasicMutex();

 public:
 void Lock();
 void Lock(const lockdep_flag f);
 void Unlock();

 bool TryLock();

 public:
 // Model BasicLockable:
 void lock()		{ return Lock(); }
 void unlock()		{ return Unlock(); }
 bool try_lock()	{ return TryLock(); }

#warning JFW race conditions waiting to happen!
 public:
 bool is_locked() const 	{ return nlocks; } 
 bool is_locked_by_me() const 	{ return is_locked() && std::this_thread::get_id() == locked_by; }

#warning JFW consider getting rid of these, or putting them in a mixin for lockdep
  void _register() {
    lockdep_id = lockdep_register(name.c_str());
  }
  void _will_lock() { // about to lock
    lockdep_id = lockdep_will_lock(name.c_str(), lockdep_id, backtrace);
  }
  void _locked() {    // just locked
    lockdep_id = lockdep_locked(name.c_str(), lockdep_id, backtrace);
  }
  void _will_unlock() {  // about to unlock
    lockdep_id = lockdep_will_unlock(name.c_str(), lockdep_id);
  }

#warning JFW consider consolidating these
  void _post_lock() {
    assert(nlocks == 0);	// JFW: makes no sense for recursive locks...
    locked_by = std::this_thread::get_id();
    nlocks++;
  }

  void _pre_unlock() {
    assert(nlocks > 0);
    --nlocks;
    assert(std::this_thread::get_id() == locked_by);
    locked_by = std::thread::id();
    assert(nlocks == 0);
  }
};

// NoLockDepMutex: A BasicMutex without LockDep: 
// 	recursive = false, lockdep = false, backtrace = false
class NoLockDepMutex 
{
 friend class Cond;

 public:
 enum class lockdep_flag : bool { enable = true, disable = false };

 // Ceph interface:
 using Locker = std::lock_guard<NoLockDepMutex>;

 private:
 std::string name;

 std::mutex mtx;

 int nlocks = 0;
#warning JFW remove me!
public: 
 std::thread::id locked_by; 

 CephContext *cct      = nullptr;
 PerfCounters *logger  = nullptr;

 private:
 NoLockDepMutex(const NoLockDepMutex&) = delete;
 void operator=(const NoLockDepMutex&) = delete;

 public:
 NoLockDepMutex(const std::string& name_, CephContext *cct_);

 NoLockDepMutex(const std::string name_)
  : NoLockDepMutex(name_, nullptr)
 {}

 ~NoLockDepMutex();

 public:
 void Lock();
// void Lock(const lockdep_flag f);
 void Unlock();

 bool TryLock();

 public:
 // Model BasicLockable:
 void lock()		{ return Lock(); }
 void unlock()		{ return Unlock(); }
 bool try_lock()	{ return TryLock(); }

#warning JFW race conditions waiting to happen!
 public:
 bool is_locked() const 	{ return nlocks; } 
 bool is_locked_by_me() const 	{ return is_locked() && std::this_thread::get_id() == locked_by; }

#warning JFW consider consolidating these
  void _post_lock() {
    assert(nlocks == 0);
    locked_by = std::this_thread::get_id();
    nlocks++;
  }

  void _pre_unlock() {
    assert(nlocks > 0);
    --nlocks;
    assert(std::this_thread::get_id() == locked_by);
    locked_by = std::thread::id();
    assert(nlocks == 0);
  }
};

// RecursiveMutex: A recursive mutex with LockDep (the default):
// 	recursive = true, lockdep = true, backtrace = false
class RecursiveMutex
{
 friend class Cond;

 public:
 enum class lockdep_flag : bool { enable = true, disable = false };

 // Ceph interface:
 using Locker = std::lock_guard<BasicMutex>;

 private:
 std::string name;

 int lockdep_id	= 0;

 std::recursive_mutex mtx;

 int nlocks = 0;

 CephContext *cct      = nullptr;
 PerfCounters *logger  = nullptr;

 private:
 RecursiveMutex(const RecursiveMutex&)   = delete;
 void operator=(const RecursiveMutex&) = delete;

 public:
 RecursiveMutex(const std::string& name_, CephContext *cct_);

 RecursiveMutex(const std::string name_)
  : RecursiveMutex(name_, nullptr)
 {}

 ~RecursiveMutex();

 public:
 void Lock();
 void Lock(const lockdep_flag f);
 void Unlock();

 bool TryLock();

 public:
 // Model BasicLockable:
 void lock()		{ return Lock(); }
 void unlock()		{ return Unlock(); }
 bool try_lock()	{ return TryLock(); }

#warning JFW race conditions waiting to happen!
 public:
 bool is_locked() const 	{ return nlocks; } 
 bool is_locked_by_me() const 	{ return is_locked(); } // JFW might need to put lock ownership in again? (cf. unique_lock<>)

#warning JFW consider consolidating these
  void _post_lock() {
    nlocks++;
  }

  void _pre_unlock() {
    assert(nlocks > 0);
    --nlocks;
  }
};

/*
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
      _post_lock();
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
