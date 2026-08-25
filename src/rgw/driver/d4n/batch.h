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

#ifndef CEPH_RGW_DRIVER_D4N_BATCH_H
#define CEPH_RGW_DRIVER_D4N_BATCH_H

#include <concepts>
#include <cstddef>
#include <deque>
#include <expected>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "common/async/yield_context.h"

/* Resumable batch transactions for D4N.
 *
 * A "plan" is an ordered sequence of transacted operations. Separate
 * entries are never atomic with one another. The Batch Transactor is a
 * feature that makes it easy to compose plan sequences that eventually converge
 * to completion. It does its best to abstract away the particulars of the back-end
 * storage plane.
 *
 * That said, there are differences (especially as we want to avoid using a back-end
 * in an entirely suboptimal way). For instance, FoundationDB applies all mutations
 * in an entry atomically. Redis MULTI/EXEC prevents interleaving, but does not roll
 * back commands when another command fails at runtime.
 *
 * Only an acknowledged, fully applied transaction is removed from the plan set. Every
 * other outcome leaves it at the front, so callers (e.g. D4N) can inspect the result
 * and choose to resume or repair explicitly. The status "applied" describes backend
 * acknowledgement; it does not make the durability guarantees of different backends
 * equivalent.
 *
 * The general contract intentionally includes only operations whose meaning
 * can be stated for every backend: put assigns one logical string value to a
 * key and erase removes a key. Backend-native operations belong in a separate
 * adapter-specific interface.
 *
 * Sadly, as not all back-ends are able to tell fun stories, there is no narration mode.
 */
namespace rgw::d4n::batch {

/*** Operations: */
struct put final {
  ::std::string key;
  ::std::string value;
};

struct erase final {
  ::std::string key;
};

using mutation = ::std::variant<put, erase>;

enum struct unknown_result_policy {
  stop,

  // Safe only when replay of the whole transaction is acceptable. Isolated
  // idempotence alone does not provide exactly-once or concurrency guarantees:
  retry_for_convergence
};

struct transaction_spec final {
  ::std::vector<mutation> mutations;
  unknown_result_policy unknown_policy = unknown_result_policy::stop;
};

template <typename MutationT>
concept mutation_operation = ::std::constructible_from<mutation, MutationT&&>;

template <typename ...MutationTs>
  requires(sizeof...(MutationTs) > 0 && (mutation_operation<MutationTs> && ...))
[[nodiscard]] transaction_spec
transaction(unknown_result_policy policy, MutationTs&& ...mutations)
{
  transaction_spec result {.unknown_policy = policy};
  result.mutations.reserve(sizeof...(MutationTs));
  (result.mutations.emplace_back(::std::forward<MutationTs>(mutations)), ...);
  return result;
}

template <typename ...MutationTs>
  requires(sizeof...(MutationTs) > 0 && (mutation_operation<MutationTs> && ...))
[[nodiscard]] transaction_spec
transaction(MutationTs&& ...mutations)
{
  return transaction(
      unknown_result_policy::stop, ::std::forward<MutationTs>(mutations)...);
}

struct plan final {
private:
  ::std::deque<transaction_spec> transactions;

public:
  plan() = default;

public:
  template <mutation_operation MutationT>
  explicit plan(MutationT&& mutation)
  {
    emplace(::std::forward<MutationT>(mutation));
  }

  explicit plan(transaction_spec transaction)
  {
    emplace(::std::move(transaction));
  }

  template <mutation_operation MutationT>
  void emplace(MutationT&& mutation)
  {
    emplace(transaction(::std::forward<MutationT>(mutation)));
  }

  void emplace(transaction_spec transaction)
  {
    if (transaction.mutations.empty()) {
      throw ::std::invalid_argument {"batch transaction must not be empty"};
    }

    transactions.push_back(::std::move(transaction));
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return transactions.empty();
  }

  [[nodiscard]] ::std::size_t size() const noexcept
  {
    return ::std::size(transactions);
  }

  [[nodiscard]] const transaction_spec& front() const
  {
    if (empty()) {
      throw ::std::logic_error {"empty batch plan has no front transaction"};
    }

    return transactions.front();
  }

  // Advance only after the front transaction is applied or repaired.
  void acknowledge_front()
  {
    if (empty()) {
      throw ::std::logic_error {
          "empty batch plan has no front transaction to acknowledge"};
    }

    transactions.pop_front();
  }
};

enum struct effect {
  // Entire transaction acknowledged:
  applied,

  // Nothing was mutated:
  not_applied,

  // Some mutations may have been applied; automatic replay is unsafe:
  partially_applied,

  // Cannot establish whether mutations were applied:
  unknown
};

enum struct failure {
  none,

  // Retrying is meaningful if the effect and transaction policy permit it:
  transient,
  permanent
};

enum struct halt_reason {
  completed,
  transaction_limit,
  retry_limit,
  failed,
  unknown_result,
  partial_result
};

struct backend_error final {
  ::std::error_code code;
  ::std::string diagnostic;
};

struct attempt_result final {
  effect observed_effect = effect::not_applied;
  failure failure_class = failure::permanent;
  ::std::optional<backend_error> error;
};

struct limits final {
  // Zero means no transaction-count limit:
  ::std::size_t transactions = 0;

  // Number of retries after the initial attempt, per front transaction:
  ::std::size_t retries = 3;
};

struct progress final {
  // Attempts include each initial attempt and every retry:
  ::std::size_t attempts = 0;
  ::std::size_t applied_transactions = 0;
  ::std::size_t remaining_transactions = 0;
  ::std::size_t transient_failures = 0;

  // Total retries performed during this invocation:
  ::std::size_t retries = 0;

  ::std::optional<backend_error> last_error;
  halt_reason reason = halt_reason::completed;
};

namespace detail {

inline void
validate_attempt_result(const attempt_result& result)
{
  const auto applied = effect::applied == result.observed_effect;
  const auto successful = failure::none == result.failure_class;

  if (applied != successful) {
    throw ::std::logic_error {
        "only an applied batch result may report no failure"};
  }

  if (applied && result.error) {
    throw ::std::logic_error {"applied batch result must not report an error"};
  }
}

template <typename SessionT>
concept execution_session = requires(SessionT& session, ::optional_yield y)
{
  { session.attempt(y) } -> ::std::same_as<attempt_result>;
};

} // namespace detail

template <typename BackendT, typename TransactionT>
concept backend = requires(
    BackendT& backend,
    const TransactionT& transaction,
    const limits& limits)
{
  { backend.begin(transaction, limits) } -> detail::execution_session;
};

template <typename BackendT, typename PlanT = plan>
class basic_transactor final {
  BackendT backend_state;

  using transaction_type =
      ::std::remove_cvref_t<decltype(::std::declval<const PlanT&>().front())>;

  [[nodiscard]] static constexpr bool transaction_limit_reached(
    const progress& result, const limits& limits) noexcept
  {
    return 0 != limits.transactions &&
           limits.transactions <= result.applied_transactions;
  }

  [[nodiscard]] static constexpr bool retry_available(
      const ::std::size_t transaction_retries,
      const limits& limits) noexcept
  {
    return limits.retries > transaction_retries;
  }

  [[nodiscard]] ::std::expected<void, halt_reason>
  execute_next_transaction(
      PlanT& work, progress& result, const limits& limits, ::optional_yield y)
  {
    auto session = backend_state.begin(work.front(), limits);
    ::std::size_t transaction_retries = 0;

    for (;;) {
      ++result.attempts;

      const auto attempt = session.attempt(y);
      detail::validate_attempt_result(attempt);

      if (attempt.error) {
        result.last_error = attempt.error;
      }

      if (failure::transient == attempt.failure_class) {
        ++result.transient_failures;
      }

      switch (attempt.observed_effect) {
        case effect::applied:
          work.acknowledge_front();
          ++result.applied_transactions;
          result.remaining_transactions = ::std::size(work);
          return {};

        case effect::not_applied:
          if (failure::transient != attempt.failure_class) {
            return ::std::unexpected {halt_reason::failed};
          }

          if (!retry_available(transaction_retries, limits)) {
            return ::std::unexpected {halt_reason::retry_limit};
          }

          ++transaction_retries;
          ++result.retries;
          continue;

        case effect::unknown:
          if (failure::transient != attempt.failure_class ||
              unknown_result_policy::retry_for_convergence !=
                  work.front().unknown_policy ||
              !retry_available(transaction_retries, limits)) {
            return ::std::unexpected {halt_reason::unknown_result};
          }

          ++transaction_retries;
          ++result.retries;
          continue;

        case effect::partially_applied:
          return ::std::unexpected {halt_reason::partial_result};
      }
    }
  }

public:
  static_assert(backend<BackendT, transaction_type>);

  explicit basic_transactor(BackendT backend) :
    backend_state(::std::move(backend))
  {}

  [[nodiscard]] progress
  operator()(PlanT& work, const limits limits = {}, ::optional_yield y = ::null_yield)
  {
    progress result {.remaining_transactions = ::std::size(work)};

    while (!work.empty()) {
      if (transaction_limit_reached(result, limits)) {
        result.reason = halt_reason::transaction_limit;
        return result;
      }

      const auto execution = execute_next_transaction(work, result, limits, y);
      if (!execution) {
        result.reason = execution.error();
        return result;
      }
    }

    return result;
  }
};

} // namespace rgw::d4n::batch

#endif // CEPH_RGW_DRIVER_D4N_BATCH_H
