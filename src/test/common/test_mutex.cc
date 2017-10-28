// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 */

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

TEST(BasicMutex, NormalAsserts) {
  BasicMutex m("Normal");
  m->Lock();
  m->Unlock();
}

TEST(BasicMutex, RecursiveWithLockdep) {
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
