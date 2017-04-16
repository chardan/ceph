#include "gtest/gtest.h"

#include "include/spinlock.h"

using ceph::spin_lock;
using ceph::spin_unlock;

TEST(SimpleSpin, Test0)
{
  std::atomic_flag lock0 = { false };
  spin_lock(&lock0);
  spin_unlock(&lock0);

  spin_lock(lock0);
  spin_unlock(lock0);
}

static std::atomic_flag lock = { false };
static uint32_t counter = 0;

static void* mythread(void *v)
{
  for (int j = 0; j < 1000000; ++j) {
    spin_lock(&lock);
    counter++;
    spin_unlock(&lock);
  }
  return NULL;
}

TEST(SimpleSpin, Test1)
{
  int ret;
  pthread_t thread1;
  pthread_t thread2;
  ret = pthread_create(&thread1, NULL, mythread, NULL);
  ASSERT_EQ(ret, 0);
  ret = pthread_create(&thread2, NULL, mythread, NULL);
  ASSERT_EQ(ret, 0);
  ret = pthread_join(thread1, NULL);
  ASSERT_EQ(ret, 0);
  ret = pthread_join(thread2, NULL);
  ASSERT_EQ(ret, 0);
  ASSERT_EQ(counter, 2000000U);
}
