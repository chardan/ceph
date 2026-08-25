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

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "driver/d4n/batch.h"

namespace batch = ::rgw::d4n::batch;

namespace {

batch::backend_error
test_error(const int code, ::std::string diagnostic)
{
  return {
      .code = {code, ::std::generic_category()},
      .diagnostic = ::std::move(diagnostic)
  };
}

batch::attempt_result
applied()
{
  return {
      .observed_effect = batch::effect::applied,
      .failure_class = batch::failure::none
  };
}

batch::attempt_result
transient(const int code)
{
  return {
      .observed_effect = batch::effect::not_applied,
      .failure_class = batch::failure::transient,
      .error = test_error(code, "transient")
  };
}

batch::attempt_result
permanent(const int code)
{
  return {
      .observed_effect = batch::effect::not_applied,
      .failure_class = batch::failure::permanent,
      .error = test_error(code, "permanent")
  };
}

batch::attempt_result
unknown(const int code)
{
  return {
      .observed_effect = batch::effect::unknown,
      .failure_class = batch::failure::transient,
      .error = test_error(code, "unknown")
  };
}

batch::attempt_result
permanent_unknown(const int code)
{
  return {
      .observed_effect = batch::effect::unknown,
      .failure_class = batch::failure::permanent,
      .error = test_error(code, "permanent unknown")
  };
}

batch::attempt_result
partial(const int code)
{
  return {
      .observed_effect = batch::effect::partially_applied,
      .failure_class = batch::failure::permanent,
      .error = test_error(code, "partial")
  };
}

struct script final {
  ::std::deque<batch::attempt_result> attempts;
  ::std::vector<::std::size_t> transaction_sizes;
};

struct scripted_session final {
  ::std::shared_ptr<script> state;

  batch::attempt_result attempt(optional_yield)
  {
    if (state->attempts.empty()) {
      throw ::std::logic_error {"script has no next attempt"};
    }

    auto result = ::std::move(state->attempts.front());
    state->attempts.pop_front();
    return result;
  }
};

struct scripted_backend final {
  ::std::shared_ptr<script> state;

  scripted_session begin(
      const batch::transaction_spec& transaction, const batch::limits&)
  {
    state->transaction_sizes.push_back(::std::size(transaction.mutations));
    return scripted_session {state};
  }
};

auto
make_scripted_transactor(
    ::std::initializer_list<batch::attempt_result> attempts,
    ::std::shared_ptr<script>& state)
{
  state = ::std::make_shared<script>();
  state->attempts = attempts;
  return batch::basic_transactor {scripted_backend {state}};
}

} // anonymous namespace

TEST_CASE("d4n batch plan owns mutations and transaction boundaries")
{
  ::std::string key = "owned-key";
  ::std::string value = "owned-value";
  batch::plan work;

  work.emplace(batch::put {key, value});
  work.emplace(
      batch::transaction(batch::erase {"old"}, batch::put {"new", "value"}));

  key.clear();
  value.clear();

  REQUIRE(2 == ::std::size(work));
  REQUIRE(1 == ::std::size(work.front().mutations));

  const auto& first = ::std::get<batch::put>(work.front().mutations.front());
  CHECK("owned-key" == first.key);
  CHECK("owned-value" == first.value);
}

TEST_CASE("d4n batch plan rejects empty transactions")
{
  batch::plan work;

  CHECK_THROWS_AS(
      work.emplace(batch::transaction_spec {}), ::std::invalid_argument);
  CHECK_THROWS_AS(work.acknowledge_front(), ::std::logic_error);
  CHECK(work.empty());
}

TEST_CASE("d4n batch completes empty plans without beginning a transaction")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({}, state);
  batch::plan work;

  const auto result = txr(work);

  CHECK(work.empty());
  CHECK(0 == result.attempts);
  CHECK(0 == result.applied_transactions);
  CHECK(0 == result.remaining_transactions);
  CHECK(batch::halt_reason::completed == result.reason);
  CHECK(state->transaction_sizes.empty());
}

TEST_CASE("d4n batch transactor removes acknowledged work in FIFO order")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({applied(), applied()}, state);
  batch::plan work;
  work.emplace(batch::put {"first", "1"});
  work.emplace(
      batch::transaction(batch::put {"second", "2"}, batch::erase {"third"}));

  const auto result = txr(work);

  CHECK(work.empty());
  CHECK(2 == result.attempts);
  CHECK(2 == result.applied_transactions);
  CHECK(0 == result.remaining_transactions);
  CHECK(0 == result.transient_failures);
  CHECK(0 == result.retries);
  CHECK_FALSE(result.last_error);
  CHECK(batch::halt_reason::completed == result.reason);
  CHECK(::std::vector<::std::size_t> {1, 2} == state->transaction_sizes);
}

TEST_CASE("d4n batch transaction limits preserve resumable work")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({applied(), applied()}, state);
  batch::plan work;
  work.emplace(batch::put {"first", "1"});
  work.emplace(batch::put {"second", "2"});

  auto result = txr(work, {.transactions = 1});

  CHECK(1 == ::std::size(work));
  CHECK(1 == result.applied_transactions);
  CHECK(1 == result.remaining_transactions);
  CHECK(batch::halt_reason::transaction_limit == result.reason);

  result = txr(work, {.transactions = 1});

  CHECK(work.empty());
  CHECK(1 == result.applied_transactions);
  CHECK(0 == result.remaining_transactions);
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n batch retries safe transient failures per transaction")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor(
      {transient(11), applied(), transient(12), applied()}, state);
  batch::plan work;
  work.emplace(batch::put {"first", "1"});
  work.emplace(batch::put {"second", "2"});

  const auto result = txr(work, {.retries = 1});

  CHECK(work.empty());
  CHECK(4 == result.attempts);
  CHECK(2 == result.applied_transactions);
  CHECK(2 == result.transient_failures);
  CHECK(2 == result.retries);
  REQUIRE(result.last_error.has_value());
  CHECK(12 == result.last_error->code.value());
  CHECK(::std::generic_category() == result.last_error->code.category());
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n batch retry exhaustion retains the front transaction")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({transient(1), transient(2)}, state);
  batch::plan work {batch::put {"key", "value"}};

  const auto result = txr(work, {.retries = 1});

  CHECK(1 == ::std::size(work));
  CHECK(2 == result.attempts);
  CHECK(0 == result.applied_transactions);
  CHECK(1 == result.remaining_transactions);
  CHECK(2 == result.transient_failures);
  CHECK(1 == result.retries);
  REQUIRE(result.last_error.has_value());
  CHECK(2 == result.last_error->code.value());
  CHECK(batch::halt_reason::retry_limit == result.reason);
}

TEST_CASE("d4n batch permanent failures retain the front transaction")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({permanent(7)}, state);
  batch::plan work {batch::erase {"key"}};

  const auto result = txr(work);

  CHECK(1 == ::std::size(work));
  CHECK(1 == result.attempts);
  CHECK(0 == result.transient_failures);
  CHECK(0 == result.retries);
  CHECK(batch::halt_reason::failed == result.reason);
}

TEST_CASE("d4n batch stops on unknown outcomes by default")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({unknown(9), applied()}, state);
  batch::plan work {batch::put {"key", "value"}};

  const auto result = txr(work);

  CHECK(1 == ::std::size(work));
  CHECK(1 == result.attempts);
  CHECK(1 == result.transient_failures);
  CHECK(0 == result.retries);
  CHECK(batch::halt_reason::unknown_result == result.reason);
  CHECK(1 == ::std::size(state->attempts));

  // Simulate external verification establishing that it did apply.
  work.acknowledge_front();
  CHECK(work.empty());
}

TEST_CASE("d4n batch retries unknown outcomes only under explicit policy")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({unknown(9), applied()}, state);
  batch::plan work {batch::transaction(
      batch::unknown_result_policy::retry_for_convergence,
      batch::put {"key", "value"})};

  const auto result = txr(work, {.retries = 1});

  CHECK(work.empty());
  CHECK(2 == result.attempts);
  CHECK(1 == result.transient_failures);
  CHECK(1 == result.retries);
  CHECK(batch::halt_reason::completed == result.reason);
}

TEST_CASE("d4n batch does not retry permanently unknown outcomes")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({permanent_unknown(9), applied()}, state);
  batch::plan work {batch::transaction(
      batch::unknown_result_policy::retry_for_convergence,
      batch::put {"key", "value"})};

  const auto result = txr(work, {.retries = 1});

  CHECK(1 == ::std::size(work));
  CHECK(1 == result.attempts);
  CHECK(0 == result.transient_failures);
  CHECK(0 == result.retries);
  CHECK(batch::halt_reason::unknown_result == result.reason);
  CHECK(1 == ::std::size(state->attempts));
}

TEST_CASE(
    "d4n batch reports unknown when convergence retry budget is exhausted")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({unknown(9)}, state);
  batch::plan work {batch::transaction(
      batch::unknown_result_policy::retry_for_convergence,
      batch::put {"key", "value"})};

  const auto result = txr(work, {.retries = 0});

  CHECK(1 == ::std::size(work));
  CHECK(1 == result.transient_failures);
  CHECK(0 == result.retries);
  CHECK(batch::halt_reason::unknown_result == result.reason);
}

TEST_CASE("d4n batch retains partially applied work")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({partial(13)}, state);
  batch::plan work {
      batch::transaction(batch::put {"first", "1"}, batch::put {"second", "2"})};

  const auto result = txr(work);

  CHECK(1 == ::std::size(work));
  CHECK(0 == result.applied_transactions);
  CHECK(batch::halt_reason::partial_result == result.reason);
}

TEST_CASE("d4n batch retains work when a backend attempt throws")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor({}, state);
  batch::plan work {batch::put {"key", "value"}};

  CHECK_THROWS_AS(txr(work), ::std::logic_error);
  CHECK(1 == ::std::size(work));
  CHECK(::std::vector<::std::size_t> {1} == state->transaction_sizes);
}

TEST_CASE("d4n batch rejects contradictory backend results")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor(
      {{.observed_effect = batch::effect::applied,
        .failure_class = batch::failure::permanent}},
      state);
  batch::plan work {batch::put {"key", "value"}};

  CHECK_THROWS_AS(txr(work), ::std::logic_error);
  CHECK(1 == ::std::size(work));
}

TEST_CASE("d4n batch rejects success reported for unapplied work")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor(
      {{.observed_effect = batch::effect::not_applied,
        .failure_class = batch::failure::none}},
      state);
  batch::plan work {batch::put {"key", "value"}};

  CHECK_THROWS_AS(txr(work), ::std::logic_error);
  CHECK(1 == ::std::size(work));
}

TEST_CASE("d4n batch rejects errors attached to applied results")
{
  ::std::shared_ptr<script> state;
  auto txr = make_scripted_transactor(
      {{.observed_effect = batch::effect::applied,
        .failure_class = batch::failure::none,
        .error = test_error(1, "contradictory")}},
      state);
  batch::plan work {batch::put {"key", "value"}};

  CHECK_THROWS_AS(txr(work), ::std::logic_error);
  CHECK(1 == ::std::size(work));
}
