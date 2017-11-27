// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 */

#include <boost/lexical_cast.hpp>

#include <common/Cond.h>
#include <common/Mutex.h>

#include "gtest/gtest.h"

#include "common/ceph_context.h"
#include "common/config.h"

#include "include/coredumpctl.h"

/*
 * Override normal ceph assert.
 * It is needed to prevent hang when we assert() and THEN still wait on lock().
 */
namespace ceph
{
  void __ceph_assert_fail(const char *assertion, const char *file, int line,
        const char *func)
  {
    throw 0;
  }
}

/********************** OLD MUTEX *************************************************/
#include <pthread.h>

#include "common/perf_counters.h"
#include "common/config.h"
#include "common/Clock.h"
#include "common/valgrind.h"


using namespace ceph;

class PerfCounters;

/*
enum {
  l_mutex_first = 999082,
  l_mutex_wait,
  l_mutex_last
};*/

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
  void operator=(const OLD_Mutex &M);
  OLD_Mutex(const OLD_Mutex &M);

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
  OLD_Mutex(const std::string &n, bool r = false, bool ld=true, bool bt=false,
	CephContext *cct = 0);
  ~OLD_Mutex();
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
    OLD_Mutex &mutex;

  public:
    explicit Locker(OLD_Mutex& m) : mutex(m) {
      mutex.Lock();
    }
    ~Locker() {
      mutex.Unlock();
    }
  };
};


OLD_Mutex::OLD_Mutex(const std::string &n, bool r, bool ld,
	     bool bt,
	     CephContext *cct) :
  name(n), id(-1), recursive(r), lockdep(ld), backtrace(bt), nlock(0),
  locked_by(0), cct(cct), logger(0)
{
  ANNOTATE_BENIGN_RACE_SIZED(&id, sizeof(id), "OLD_Mutex lockdep id");
  ANNOTATE_BENIGN_RACE_SIZED(&nlock, sizeof(nlock), "OLD_Mutex nlock");
  ANNOTATE_BENIGN_RACE_SIZED(&locked_by, sizeof(locked_by), "OLD_Mutex locked_by");
  if (cct) {
    PerfCountersBuilder b(cct, string("mutex-") + name,
			  l_mutex_first, l_mutex_last);
    b.add_time_avg(l_mutex_wait, "wait", "Average time of mutex in locked state");
    logger = b.create_perf_counters();
    cct->get_perfcounters_collection()->add(logger);
    logger->set(l_mutex_wait, 0);
  }
  if (recursive) {
    // Mutexes of type PTHREAD_MUTEX_RECURSIVE do all the same checks as
    // mutexes of type PTHREAD_MUTEX_ERRORCHECK.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_m,&attr);
    pthread_mutexattr_destroy(&attr);
    if (lockdep && g_lockdep)
      _register();
  }
  else if (lockdep) {
    // If the mutex type is PTHREAD_MUTEX_ERRORCHECK, then error checking
    // shall be provided. If a thread attempts to relock a mutex that it
    // has already locked, an error shall be returned. If a thread
    // attempts to unlock a mutex that it has not locked or a mutex which
    // is unlocked, an error shall be returned.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&_m, &attr);
    pthread_mutexattr_destroy(&attr);
    if (g_lockdep)
      _register();
  }
  else {
    // If the mutex type is PTHREAD_MUTEX_DEFAULT, attempting to recursively
    // lock the mutex results in undefined behavior. Attempting to unlock the
    // mutex if it was not locked by the calling thread results in undefined
    // behavior. Attempting to unlock the mutex if it is not locked results in
    // undefined behavior.
    pthread_mutex_init(&_m, NULL);
  }
}

OLD_Mutex::~OLD_Mutex() {
  assert(nlock == 0);

  // helgrind gets confused by condition wakeups leading to mutex destruction
  ANNOTATE_BENIGN_RACE_SIZED(&_m, sizeof(_m), "OLD_Mutex primitive");
  pthread_mutex_destroy(&_m);

  if (cct && logger) {
    cct->get_perfcounters_collection()->remove(logger);
    delete logger;
  }
  if (lockdep && g_lockdep) {
    lockdep_unregister(id);
  }
}

void OLD_Mutex::Lock(bool no_lockdep) {
  int r;

  if (lockdep && g_lockdep && !no_lockdep && !recursive) _will_lock();

  if (logger && cct && cct->_conf->mutex_perf_counter) {
    utime_t start;
    // instrumented mutex enabled
    start = ceph_clock_now();
    if (TryLock()) {
      goto out;
    }

    r = pthread_mutex_lock(&_m);

    logger->tinc(l_mutex_wait,
		 ceph_clock_now() - start);
  } else {
    r = pthread_mutex_lock(&_m);
  }

  assert(r == 0);
  if (lockdep && g_lockdep) _locked();
  _post_lock();

out:
  ;
}

void OLD_Mutex::Unlock() {
  _pre_unlock();
  if (lockdep && g_lockdep) _will_unlock();
  int r = pthread_mutex_unlock(&_m);
  assert(r == 0);
}

/**************************** END OLD MUTEX ********************************/


static CephContext* cct;

static void do_init() {
  if (cct == nullptr) {
    cct = new CephContext(0);
    lockdep_register_ceph_context(cct);
   }
}

static void disable_lockdep() {
  if (cct) {
    lockdep_unregister_ceph_context(cct);
    cct->put();
    cct = nullptr;
  }
}

template <typename MutexT>
void mutex_bench(const char *name)
{
  using namespace std::chrono;

  auto i = 1;

  const long long trials = 50000000000;

auto b = steady_clock::now();

  // I slightly try to confuse the optimizer, but fail...
  for(long long i = 0; trials < i; ++i) {
    MutexT m("x");
//    std::lock_guard<MutexT> lg(m);

    m.Lock();
    i %= rand() ^ (trials - (rand() % 1000));

    { 
    MutexT mx("mx");

    m.Unlock();

    i %= rand() ^ (trials - (rand() % 3000));

    auto j = i / 2;
    i += j;

    m.Lock();

    mx.Unlock();
    }

    i %= rand() ^ (trials - (rand() % 2000));
    m.Unlock();

    i %= rand() ^ (trials - (rand() % 3000));
  }

  auto e = steady_clock::now();

  cout << i << '\n';  

  auto d = e - b;

  cout << "JFW: " << name << " time: " << chrono::duration<double, milli>(d).count() << " ms" << '\n';
}

template <typename MutexT>
void recursive_mutex_bench(MutexT& m, const char *name)
{
  using namespace std::chrono;

  auto i = 1;

  const long long trials = 50000000000;

auto b = steady_clock::now();

  // I slightly try to confuse the optimizer, but fail...
  for(long long i = 0; trials < i; ++i) {
    i %= rand() ^ (trials - (rand() % 3000));
	m.Lock();
  }

  for(long long i = 0; trials < i; ++i) {
    i %= rand() ^ (trials - (rand() % 3000));
	m.Unlock();
  }

  auto e = steady_clock::now();

  cout << i << '\n';  

  auto d = e - b;

  cout << "JFW: " << name << " time: " << chrono::duration<double, milli>(d).count() << " ms" << '\n';
}

TEST(BasicMutex, NormalBench) {
 mutex_bench<OLD_Mutex>("OLD_Mutex");
 mutex_bench<BasicMutex>("BasicMutex");
 mutex_bench<NoLockDepMutex>("NoLockDepMutex");
 mutex_bench<RecursiveMutex>("RecursiveMutex");

 OLD_Mutex orm("OLD_MutexR", true); // recursive, lockdep
 OLD_Mutex orm2("OLD_MutexR_NLD", true, false); // recursive, no lockdep
 RecursiveMutex r("RecursiveMutex");

 recursive_mutex_bench(orm, "OLD_MutexR");
 recursive_mutex_bench(orm2, "OLD_MutexR_NLD");
 recursive_mutex_bench(r, "RecursiveMutex");
}

TEST(BasicMutex, NormalWithLockdep) {
  BasicMutex m("Normal");
  m.Lock();
  m.Unlock();
}

TEST(BasicMutex, NormalWithNoLockdep) {
  NoLockDepMutex m("Normal");
  m.Lock();
  m.Unlock();
}

TEST(BasicMutex, RecursiveWithLockdep) {

  // Activate the global lockdep flag:
  do_init();

  auto* m = new RecursiveMutex("Recursive1");

  m->Lock();
  m->Lock();
  m->Unlock();
  m->Unlock();

  delete m;
}

TEST(BasicMutex, RecursiveWithoutLockdep) {
  disable_lockdep();
  auto* m = new RecursiveMutex("Recursive2");
  m->Lock();
  m->Lock();
  m->Unlock();
  m->Unlock();
  delete m;
}

TEST(BasicMutex, DeleteLocked) {
  auto* m = new BasicMutex("Recursive3"); // JFW: not sure why this is called recursive...
  m->Lock();
  PrCtl unset_dumpable;
  EXPECT_DEATH(delete m,".*");
}

TEST(BasicCond, BasicCondTest) {

BasicMutex m("test mutex");

Cond c;
try {

c.Wait(m);
}
catch(std::exception& e)
{
 std::cout << "JFW: exception: " << e.what() << '\n';
}
catch(...) {
 std::cout << "JFW: exception-- unknown type!\n";
 throw;
}

}
