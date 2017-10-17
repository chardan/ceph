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

#include "include/assert.h"
#include "lockdep.h"
#include "common/ceph_context.h"

#include <pthread.h>

using namespace ceph;

class PerfCounters;

enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};

class Mutex {
  friend class Cond;
  
  private:
  std::mutex mtx;

  std::string name;

  int id = -1;

  bool lockdep = true;
  bool gather_backtrace = false;	// gather backtrace on lock acquisition

// JFW: strongly suspect this needs to be atomic-- and even at that...
  int nlock = 0;	

  CephContext *cct;
  PerfCounters *logger;

  Mutex(Mutex&&)		= delete;
  Mutex(const Mutex&)		= delete;
  void operator=(const Mutex &)	= delete;

  private:
  void _register() {
    id = lockdep_register(name.c_str());
  }
  void _will_lock() { // about to lock
    id = lockdep_will_lock(name.c_str(), id, gather_backtrace);
  }
  void _locked() {    // just locked
    id = lockdep_locked(name.c_str(), id, gather_backtrace);
  }
  void _will_unlock() {  // about to unlock
    id = lockdep_will_unlock(name.c_str(), id);
  }

JFW
  public:
  Mutex(const std::string& name_)
   : name(name_)
  {
	ANNOTATE_BENIGN_RACE_SIZED(&id, sizeof(id), "Mutex lockdep id");
	ANNOTATE_BENIGN_RACE_SIZED(&nlock, sizeof(nlock), "Mutex nlock");
	ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "Mutex locked_by");

// JFW: see if PTHREAD_MUTEX_ERRORCHECK matters here...
	if (g_lockdep)	// JFW: not lockdep && g_lockdep, like elsewhere..?
         _register();
  }

  ~Mutex()
  {
   assert(nlock == 0);

   // helgrind gets confused by condition wakeups leading to mutex destruction
   ANNOTATE_BENIGN_RACE_SIZED(&_m, sizeof(_m), "Mutex primitive");

   if (lockdep && g_lockdep)
    lockdep_unregister(id);
  }

#warning JFW get rid of these 
  bool is_locked() const noexcept { return nlock > 0; }

  bool is_locked_by_me() const noexcept {
    return nlock > 0 && locked_by == mtx::native_handle();
  }

  bool TryLock() {
    if(mtx.try_lock())
     return true;

    if (lockdep && g_lockdep)
     _locked();

    _post_lock();

    return false;
  }

  void Lock(bool no_lockdep=false);

  void _post_lock() {
    assert(nlock == 0);
    nlock++;
  }

#warning JFW get rid of these
  void _pre_unlock() {
    assert(nlock > 0);
    --nlock;
    assert(mtx.native_handl() == pthread_self());
    locked_by = 0;
    assert(nlock == 0);
  }
  void Unlock();

#warning JFW get rid of this
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

class Old_Mutex {
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
  Old_Mutex(const std::string &n, bool r = false, bool ld=true, bool bt=false,
	CephContext *cct = 0);
  ~Old_Mutex();
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


#endif
