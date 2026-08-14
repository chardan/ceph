// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- // vim: ts=8 sw=2 smarttab ft=cpp
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 International Business Machines Corp. (IBM)
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "driver/d4n/batch.h"
#include "test/rgw/test_fdb_common.h"

#include <array>
#include <tuple>
#include <chrono>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <iterator>
#include <stdexcept>

using namespace std::chrono_literals;

namespace batch = rgw::d4n::batch;

static_assert(batch::limits{}.transaction_timeout == 3s);
static_assert(0 == batch::limits{}.transactions);
static_assert(batch::limits{}.retries == 3);
static_assert(!batch::progress{}.last_error);
static_assert(batch::progress{}.reason == batch::halt_reason::completed);

namespace {

void set_reusable_free_function_key(lfdb::transaction_handle& txn)
{
  lfdb::set(txn, test_key("batch-reusable-free-function"), "free");
}

struct reusable_object_op final {
  std::string key;
  std::string value;

  void operator()(lfdb::transaction_handle& txn)
  {
    lfdb::set(txn, key, value);
  }
};

} // anonymous namespace

TEST_CASE("d4n batch limits have conservative defaults")
{
  const batch::limits limits;

  CHECK(limits.transaction_timeout == 3s);
  CHECK(0 == limits.transactions);
  CHECK(limits.retries == 3);
}

TEST_CASE("d4n batch plan queues ordered work")
{
  batch::plan plan;

  CHECK(plan.empty());
  CHECK(0 == plan.size());

  plan.emplace([](auto&) {});

  batch::plan::op_t op = [](auto&) {};
  plan.push(std::move(op));

  CHECK(!plan.empty());
  CHECK(plan.size() == 2);
}

TEST_CASE("d4n batch plan rejects empty erased operations")
{
  batch::plan plan;
  batch::plan::op_t op;

  CHECK_THROWS_AS(plan.push(std::move(op)), std::invalid_argument);
}

TEST_CASE("d4n batch plan accepts grouped operations")
{
  batch::plan plan {
    [](auto&) {},
    [](auto&) {},
  };

  CHECK(plan.size() == 2);

  plan.emplace_all(
    [](auto&) {},
    [](auto&) {});

  CHECK(plan.size() == 4);
}

TEST_CASE("d4n batch plan appends multiple operations with emplace")
{
  batch::plan plan;

  plan.emplace(
    [](auto&) {},
    [](auto&) {},
    [](auto&) {});

  CHECK(plan.size() == 3);
}

TEST_CASE("d4n batch transaction composes shared transaction work")
{
  batch::plan plan;

  plan.emplace(batch::transaction(
    [](auto&) {},
    [](auto&) {},
    [](auto&) {}));

  CHECK(plan.size() == 1);
}

TEST_CASE("d4n batch transaction accepts move-only work")
{
  janitor j;
  batch::plan plan;
  const auto key = test_key("batch-shared-move-only");
  auto value = std::make_unique<int>(7);

  plan.emplace(batch::transaction(
    [key, value = std::move(value)](auto& txn) {
      lfdb::set(txn, key, std::to_string(*value));
    }));

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, key, out));
  CHECK(out == "7");
  CHECK(plan.empty());
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch transaction ignores operation return values")
{
  janitor j;
  batch::plan plan;
  const auto key = test_key("batch-shared-returns");

  plan.emplace(batch::transaction(
    [&key](auto& txn) {
      lfdb::set(txn, key, "value");
      return 7;
    }));

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, key, out));
  CHECK(out == "value");
  CHECK(plan.empty());
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch transaction accepts reusable transaction operations")
{
  janitor j;
  batch::plan plan;
  const auto free_function_key = test_key("batch-reusable-free-function");
  const auto function_object_key = test_key("batch-reusable-object");

  plan.emplace(batch::transaction(
    set_reusable_free_function_key,
    reusable_object_op {
      .key = function_object_key,
      .value = "object",
    }));

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, free_function_key, out));
  CHECK(out == "free");
  CHECK(lfdb::get(j, function_object_key, out));
  CHECK(out == "object");
  CHECK(plan.empty());
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch plan appends tuple operations")
{
  batch::plan plan;
  auto ops = std::tuple {
    [](auto&) {},
    [](auto&) {},
  };

  plan.append(std::move(ops));

  CHECK(plan.size() == 2);
}

TEST_CASE("d4n batch plan accepts empty tuple append")
{
  batch::plan plan;

  plan.append(std::tuple {});

  CHECK(plan.empty());
}

TEST_CASE("d4n batch plan appends range operations")
{
  batch::plan plan;
  constexpr auto keys = std::array {1, 2, 3};

  plan.append(keys | std::views::transform([](const auto key) {
    return [key](auto&) {
      (void)key;
    };
  }));

  CHECK(plan.size() == std::size(keys));
}

TEST_CASE("d4n batch executor reports an empty plan")
{
  batch::plan plan;
  ceph::libfdb::database_handle dbh;

  auto progress = plan(dbh);

  CHECK(0 == progress.attempts);
  CHECK(0 == progress.committed_ops);
  CHECK(0 == progress.remaining_ops);
  CHECK(0 == progress.committed_transactions);
  CHECK(0 == progress.retryable_failures);
  CHECK(0 == progress.retries);
  CHECK_FALSE(progress.last_error);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch plan rejects invalid limits before transaction creation")
{
  batch::plan plan;
  ceph::libfdb::database_handle dbh;

  CHECK_THROWS_AS(plan(dbh, {.transaction_timeout = 0ms}), std::invalid_argument);
}

TEST_CASE("d4n batch plan leaves work queued after database-handle errors")
{
  batch::plan plan;
  ceph::libfdb::database_handle dbh;

  plan.emplace([](auto& txn) {
    lfdb::set(txn, test_key("unreachable"), "value");
  });

  CHECK_THROWS_AS(plan(dbh), std::invalid_argument);
  CHECK(plan.size() == 1);
}

TEST_CASE("d4n batch plan commits queued work")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-success-0");
  const auto second = test_key("batch-success-1");

  plan.emplace_all(
    [&first](auto& txn) {
      lfdb::set(txn, first, "first");
    },
    [&second](auto& txn) {
      lfdb::set(txn, second, "second");
    });

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, first, out));
  CHECK(out == "first");
  CHECK(lfdb::get(j, second, out));
  CHECK(out == "second");

  CHECK(plan.empty());
  CHECK(progress.attempts == 2);
  CHECK(progress.committed_ops == 2);
  CHECK(0 == progress.remaining_ops);
  CHECK(progress.committed_transactions == 2);
  CHECK(0 == progress.retryable_failures);
  CHECK(0 == progress.retries);
  CHECK_FALSE(progress.last_error);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch transaction commits inner work together")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-shared-success-0");
  const auto second = test_key("batch-shared-success-1");

  plan.emplace(batch::transaction(
    [&first](auto& txn) {
      lfdb::set(txn, first, "first");
    },
    [&second](auto& txn) {
      lfdb::set(txn, second, "second");
    }));

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, first, out));
  CHECK(out == "first");
  CHECK(lfdb::get(j, second, out));
  CHECK(out == "second");

  CHECK(plan.empty());
  CHECK(progress.attempts == 1);
  CHECK(progress.committed_ops == 1);
  CHECK(0 == progress.remaining_ops);
  CHECK(progress.committed_transactions == 1);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch transaction leaves no partial commit after inner failure")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-shared-throw-0");
  const auto second = test_key("batch-shared-throw-1");

  plan.emplace(batch::transaction(
    [&first](auto& txn) {
      lfdb::set(txn, first, "first");
    },
    [](auto&) {
      throw std::runtime_error("shared transaction failed");
    },
    [&second](auto& txn) {
      lfdb::set(txn, second, "second");
    }));

  CHECK_THROWS_AS(plan(j), std::runtime_error);

  std::string out;
  CHECK_FALSE(lfdb::get(j, first, out));
  CHECK_FALSE(lfdb::get(j, second, out));
  CHECK(plan.size() == 1);
}

TEST_CASE("d4n batch transaction replays inner work after commit conflict")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-shared-conflict-0");
  const auto second = test_key("batch-shared-conflict-1");
  bool forced_conflict = false;

  lfdb::set(j, first, "initial");

  plan.emplace(batch::transaction(
    [&j, &first, &forced_conflict](auto& txn) {
      std::string out;
      if (!lfdb::get(txn, first, out)) {
        throw std::runtime_error("expected key does not exist");
      }

      // The whole shared operation replays until its transaction commits:
      if (!forced_conflict) {
        forced_conflict = true;
        lfdb::set(j, first, "conflict");
      }

      lfdb::set(txn, first, "final");
    },
    [&second](auto& txn) {
      lfdb::set(txn, second, "second");
    }));

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, first, out));
  CHECK(out == "final");
  CHECK(lfdb::get(j, second, out));
  CHECK(out == "second");

  CHECK(plan.empty());
  CHECK(progress.attempts == 2);
  CHECK(progress.committed_ops == 1);
  CHECK(0 == progress.remaining_ops);
  CHECK(progress.committed_transactions == 1);
  CHECK(progress.retryable_failures == 1);
  CHECK(progress.retries == 1);
  CHECK(progress.last_error);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch plan stops at transaction limit and resumes")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-limit-0");
  const auto second = test_key("batch-limit-1");

  plan.emplace_all(
    [&first](auto& txn) {
      lfdb::set(txn, first, "first");
    },
    [&second](auto& txn) {
      lfdb::set(txn, second, "second");
    });

  auto progress = plan(j, {.transactions = 1});

  std::string out;
  CHECK(lfdb::get(j, first, out));
  CHECK(out == "first");
  CHECK_FALSE(lfdb::get(j, second, out));

  CHECK(plan.size() == 1);
  CHECK(progress.attempts == 1);
  CHECK(progress.committed_ops == 1);
  CHECK(progress.remaining_ops == 1);
  CHECK(progress.committed_transactions == 1);
  CHECK(progress.reason == batch::halt_reason::limit);

  progress = plan(j, {.transactions = 1});

  CHECK(lfdb::get(j, second, out));
  CHECK(out == "second");
  CHECK(plan.empty());
  CHECK(progress.attempts == 1);
  CHECK(progress.committed_ops == 1);
  CHECK(0 == progress.remaining_ops);
  CHECK(progress.committed_transactions == 1);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch plan leaves failed application work queued")
{
  janitor j;
  batch::plan plan;
  const auto first = test_key("batch-throw-0");

  plan.emplace_all(
    [&first](auto& txn) {
      lfdb::set(txn, first, "first");
    },
    [](auto&) {
      throw std::runtime_error("batch operation failed");
    });

  CHECK_THROWS_AS(plan(j, {.transactions = 2}), std::runtime_error);

  std::string out;
  CHECK(lfdb::get(j, first, out));
  CHECK(out == "first");
  CHECK(plan.size() == 1);
}

TEST_CASE("d4n batch plan replays retryable commit failures")
{
  janitor j;
  batch::plan plan;
  const auto key = test_key("batch-conflict");
  bool forced_conflict = false;

  lfdb::set(j, key, "initial");

  plan.emplace([&j, &key, &forced_conflict](auto& txn) {
    std::string out;
    if (!lfdb::get(txn, key, out)) {
      throw std::runtime_error("expected key does not exist");
    }

    // The outside write forces a commit conflict on the first attempt:
    if (!forced_conflict) {
      forced_conflict = true;
      lfdb::set(j, key, "conflict");
    }

    lfdb::set(txn, key, "final");
  });

  const auto progress = plan(j);

  std::string out;
  CHECK(lfdb::get(j, key, out));
  CHECK(out == "final");

  CHECK(plan.empty());
  CHECK(progress.attempts == 2);
  CHECK(progress.committed_ops == 1);
  CHECK(0 == progress.remaining_ops);
  CHECK(progress.committed_transactions == 1);
  CHECK(progress.retryable_failures == 1);
  CHECK(progress.retries == 1);
  CHECK(progress.last_error);
  CHECK(progress.reason == batch::halt_reason::completed);
}

TEST_CASE("d4n batch plan reports retry exhaustion")
{
  janitor j;
  batch::plan plan;
  const auto key = test_key("batch-retry-limit");
  bool forced_conflict = false;

  lfdb::set(j, key, "initial");

  plan.emplace([&j, &key, &forced_conflict](auto& txn) {
    std::string out;
    if (!lfdb::get(txn, key, out)) {
      throw std::runtime_error("expected key does not exist");
    }

    // The retry limit is zero, so the first retryable failure stops the plan:
    if (!forced_conflict) {
      forced_conflict = true;
      lfdb::set(j, key, "conflict");
    }

    lfdb::set(txn, key, "final");
  });

  const auto progress = plan(j, {.retries = 0});

  std::string out;
  CHECK(lfdb::get(j, key, out));
  CHECK(out == "conflict");

  CHECK(plan.size() == 1);
  CHECK(progress.attempts == 1);
  CHECK(0 == progress.committed_ops);
  CHECK(progress.remaining_ops == 1);
  CHECK(0 == progress.committed_transactions);
  CHECK(progress.retryable_failures == 1);
  CHECK(0 == progress.retries);
  CHECK(progress.last_error);
  CHECK(progress.reason == batch::halt_reason::retry_limit);
}

TEST_CASE("d4n batch plan preserves work after transaction timeout errors")
{
  janitor j;
  batch::plan plan;
  const auto key = test_key("batch-timeout");

  plan.emplace([&key](auto& txn) {
    lfdb::set(txn, key, "too-late");
    std::this_thread::sleep_for(10ms);
  });

  CHECK_THROWS_AS(plan(j, {
    .transaction_timeout = 1ms,
    .retries = 0,
  }), lfdb::libfdb_exception);

  std::string out;
  CHECK_FALSE(lfdb::get(j, key, out));
  CHECK(plan.size() == 1);
}

int main(int argc, char *argv[])
{
  const auto result = Catch::Session().run(argc, argv);

  ceph::libfdb::shutdown_libfdb();

  return result;
}
