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

#include <chrono>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "driver/d4n/batch_fdb.h"
#include "driver/d4n/batch_fdb_native.h"
#include "test/rgw/test_fdb_common.h"

using namespace ::std::chrono_literals;

namespace batch = ::rgw::d4n::batch;
namespace fdb_native = ::rgw::d4n::batch::fdb_native;

namespace {

bool
foundationdb_available()
{
  static const bool available = [] {
    try {
      const auto dbh = lfdb::create_database(lfdb::database_options {
          {FDB_DB_OPTION_TRANSACTION_TIMEOUT, ::std::int64_t {100}}});
      auto txn = lfdb::make_transaction(dbh);
      lfdb::set(txn, test_key("general-batch-availability"), "available");
      return lfdb::commit(txn);
    } catch (const lfdb::libfdb_exception&) {
      return false;
    }
  }();

  return available;
}

void
require_foundationdb()
{
  if (!foundationdb_available()) {
    SKIP("FoundationDB is not available for live D4N batch tests");
  }
}

} // anonymous namespace

TEST_CASE("d4n FDB batch classifies commit outcomes conservatively")
{
  constexpr fdb_error_t not_committed = 1020;
  constexpr fdb_error_t maybe_committed = 1021;

  REQUIRE(
      0 != fdb_error_predicate(
               FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, not_committed));
  REQUIRE(
      0 !=
      fdb_error_predicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, maybe_committed));

  const auto safe_retry =
      batch::detail::classify_fdb_commit_error(not_committed);
  CHECK(batch::effect::not_applied == safe_retry.observed_effect);
  CHECK(batch::failure::transient == safe_retry.failure_class);

  const auto unknown = batch::detail::classify_fdb_commit_error(maybe_committed);
  CHECK(batch::effect::unknown == unknown.observed_effect);
  REQUIRE(unknown.error.has_value());
  CHECK(maybe_committed == unknown.error->code.value());
  CHECK(batch::fdb_category() == unknown.error->code.category());
  CHECK(fdb_get_error(maybe_committed) == unknown.error->diagnostic);

  const auto body_retry = batch::detail::classify_fdb_body_error(not_committed);
  CHECK(batch::effect::not_applied == body_retry.observed_effect);
  CHECK(batch::failure::transient == body_retry.failure_class);

  const auto body_failure = batch::detail::classify_fdb_body_error(-1);
  CHECK(batch::effect::not_applied == body_failure.observed_effect);
  CHECK(batch::failure::permanent == body_failure.failure_class);
}

TEST_CASE("d4n FDB batch validates adapter configuration")
{
  ::ceph::libfdb::database_handle dbh;

  CHECK_THROWS_AS(
      batch::make_transactor(dbh, {.transaction_timeout = 0ms}),
      ::std::invalid_argument);

  auto txr = batch::make_transactor(dbh);
  batch::plan empty;
  const auto empty_result = txr(empty);
  CHECK(batch::halt_reason::completed == empty_result.reason);

  batch::plan work {batch::put {"key", "value"}};
  CHECK_THROWS_AS(txr(work), ::std::invalid_argument);
  CHECK(1 == ::std::size(work));
}

TEST_CASE("d4n FDB batch applies grouped put and erase atomically")
{
  require_foundationdb();

  janitor database;
  const auto put_key = test_key("general-batch-put");
  const auto erase_key = test_key("general-batch-erase");
  lfdb::set(database, erase_key, "old");

  auto txr = batch::make_transactor(database);
  batch::plan work {
      batch::transaction(batch::put {put_key, "new"}, batch::erase {erase_key})};

  const auto result = txr(work);

  ::std::string value;
  CHECK(lfdb::get(database, put_key, value));
  CHECK("new" == value);
  CHECK_FALSE(lfdb::get(database, erase_key, value));
  CHECK(work.empty());
  CHECK(1 == result.attempts);
  CHECK(1 == result.applied_transactions);
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n FDB batch plans remain resumable at transaction limits")
{
  require_foundationdb();

  janitor database;
  const auto first = test_key("general-batch-limit-first");
  const auto second = test_key("general-batch-limit-second");
  auto txr = batch::make_transactor(database);
  batch::plan work;
  work.emplace(batch::put {first, "first"});
  work.emplace(batch::put {second, "second"});

  auto result = txr(work, {.transactions = 1});

  ::std::string value;
  CHECK(lfdb::get(database, first, value));
  CHECK("first" == value);
  CHECK_FALSE(lfdb::get(database, second, value));
  CHECK(1 == ::std::size(work));
  CHECK(batch::halt_reason::transaction_limit == result.reason);

  result = txr(work, {.transactions = 1});

  CHECK(lfdb::get(database, second, value));
  CHECK("second" == value);
  CHECK(work.empty());
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n native FDB batch supports callback transactions")
{
  require_foundationdb();

  janitor database;
  const auto put_key = test_key("native-batch-put");
  const auto erase_key = test_key("native-batch-erase");
  lfdb::set(database, erase_key, "old");

  auto txr = fdb_native::make_transactor(database);
  fdb_native::plan work {fdb_native::transaction(
      [&put_key](lfdb::transaction_handle& transaction) {
        lfdb::set(transaction, put_key, "new");
      },
      [&erase_key](lfdb::transaction_handle& transaction) {
        lfdb::erase(transaction, erase_key);
      })};

  const auto result = txr(work);

  ::std::string value;
  CHECK(lfdb::get(database, put_key, value));
  CHECK("new" == value);
  CHECK_FALSE(lfdb::get(database, erase_key, value));
  CHECK(work.empty());
  CHECK(1 == result.applied_transactions);
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n native FDB batch replays retryable operation-body errors")
{
  require_foundationdb();

  janitor database;
  const auto key = test_key("native-batch-body-replay");
  bool inject_retryable_error = true;
  auto txr = fdb_native::make_transactor(database);
  fdb_native::plan work {fdb_native::transaction(
      [&inject_retryable_error, &key](lfdb::transaction_handle& transaction) {
        if (::std::exchange(inject_retryable_error, false)) {
          throw lfdb::libfdb_exception {1020};
        }

        lfdb::set(transaction, key, "applied");
      })};

  const auto result = txr(work, {.retries = 1});

  ::std::string value;
  CHECK(lfdb::get(database, key, value));
  CHECK("applied" == value);
  CHECK(work.empty());
  CHECK(2 == result.attempts);
  CHECK(1 == result.retries);
  CHECK(1 == result.transient_failures);
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n native FDB batch replays commit conflicts")
{
  require_foundationdb();

  janitor database;
  const auto key = test_key("native-batch-commit-conflict");
  lfdb::set(database, key, "initial");

  bool inject_conflict = true;
  auto txr = fdb_native::make_transactor(database);
  fdb_native::plan work {fdb_native::transaction(
      [&database, &inject_conflict, &key](
          lfdb::transaction_handle& transaction) {
        ::std::string value;
        lfdb::get(transaction, key, value);
        lfdb::set(transaction, key, "batch");

        if (::std::exchange(inject_conflict, false)) {
          lfdb::set(database, key, "conflict");
        }
      })};

  const auto result = txr(work, {.retries = 1});

  ::std::string value;
  CHECK(lfdb::get(database, key, value));
  CHECK("batch" == value);
  CHECK(work.empty());
  CHECK(2 == result.attempts);
  CHECK(1 == result.retries);
  CHECK(1 == result.transient_failures);
  REQUIRE(result.last_error.has_value());
  CHECK(
      0 != fdb_error_predicate(
               FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED,
               result.last_error->code.value()));
  CHECK(batch::fdb_category() == result.last_error->code.category());
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n native FDB batch rejects empty callbacks")
{
  fdb_native::plan work;
  fdb_native::operation empty_operation;

  CHECK_THROWS_AS(
      work.emplace(fdb_native::transaction_spec {}), ::std::invalid_argument);
  CHECK_THROWS_AS(
      work.emplace(::std::move(empty_operation)), ::std::invalid_argument);
  CHECK_THROWS_AS(work.acknowledge_front(), ::std::logic_error);
  CHECK(work.empty());
}

int
main(int argc, char *argv[])
{
  const auto result = ::Catch::Session().run(argc, argv);
  ::ceph::libfdb::shutdown_libfdb();
  return result;
}
