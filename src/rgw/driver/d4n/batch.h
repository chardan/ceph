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

#ifdef WITH_RADOSGW_FDB

#include "rgw/ceph_fdb.h"

#include <deque>
#include <tuple>
#include <chrono>
#include <ranges>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <concepts>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <functional>
#include <type_traits>

/* Batch transaction support for D4N.
 *
 * This module provides a Batch Transactor that executes a Batch Plan. A Batch
 * Plan is a sequence of operations that you expect to commit in their own
 * transactions, while still making progress across unexpected but retryable
 * failures.
 *
 * FoundationDB limits broadly come in two flavors: a limit against how long a
 * transaction is held and limits against the work represented by a transaction
 * (for instance, size). The focus here is on dealing with the former.
 *
 * This is a convergence tool for cache operations, not a way to retain
 * atomicity across multiple transactions.
 *
*/
namespace rgw::d4n::batch {

namespace detail {

using txn_t = ceph::libfdb::transaction_handle;
using op_t = std::move_only_function<void(txn_t&)>;

template <typename T>
concept operation = std::constructible_from<op_t, T&&>;

template <typename T>
concept transaction_operation =
  std::constructible_from<std::decay_t<T>, T&&> &&
  std::invocable<std::decay_t<T>&, txn_t&>;

template <typename T>
concept tuple_like = requires {
  typename std::tuple_size<std::remove_cvref_t<T>>::type;
};

} // namespace detail

// Why did the transaction stop?
enum class halt_reason {
  completed,
  limit,
  retry_limit,
};

struct limits final {
  std::chrono::milliseconds transaction_timeout = std::chrono::seconds{3};

  // 0 == no specific transaction-count limit:
  std::size_t transactions = 0;

  // After a failure, we retry up to this many times:
  std::size_t retries = 3;
};

struct progress final {
  std::size_t attempts = 0;
  std::size_t committed_ops = 0;
  std::size_t remaining_ops = 0;
  std::size_t committed_transactions = 0;
  std::size_t retryable_failures = 0;
  std::size_t retries = 0;

  std::optional<fdb_error_t> last_error;

  halt_reason reason = halt_reason::completed;
};

// Ordered work plan: committed operations are consumed, uncommitted operations remain.
struct plan final {
public:
  using op_t = detail::op_t;

private:
  std::deque<op_t> ops;

  static void require_op(const op_t& op)
  {
    if (!op) {
      throw std::invalid_argument{"batch operation must not be empty"};
    }
  }

  static void require_limits(const limits& limits)
  {
    if (limits.transaction_timeout <= std::chrono::milliseconds{0}) {
      throw std::invalid_argument{"batch transaction timeout must be positive"};
    }
  }

  static void require_database(const ceph::libfdb::database_handle& dbh)
  {
    if (!dbh || !*dbh) {
      throw std::invalid_argument{"batch database handle must be open"};
    }
  }

  void push_erased(op_t op)
  {
    require_op(op);

    ops.push_back(std::move(op));
  }

  [[nodiscard]] static ceph::libfdb::transaction_options
  transaction_options_for(const limits& limits)
  {
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      limits.transaction_timeout);

    return {
      { FDB_TR_OPTION_TIMEOUT,
        static_cast<std::int64_t>(timeout.count()) }
    };
  }

  [[nodiscard]] static constexpr bool retry_exhausted(const progress& result,
                                                      const limits& limits) noexcept
  {
    return result.retries >= limits.retries;
  }

  [[nodiscard]] static constexpr bool transaction_limit_reached(const progress& result,
                                                                const limits& limits) noexcept
  {
    return limits.transactions != 0 && result.committed_transactions >= limits.transactions;
  }

  [[nodiscard]] static bool account_retry(progress& result,
                                          const limits& limits,
                                          const fdb_error_t r)
  {
    result.last_error = r;
    ++result.retryable_failures;

    if (retry_exhausted(result, limits)) {
      result.reason = halt_reason::retry_limit;
      return false;
    }

    ++result.retries;
    return true;
  }

public:
  plan() = default;

  template <typename ...OpTs>
  requires (sizeof...(OpTs) > 0 && (detail::operation<OpTs> && ...))
  explicit plan(OpTs&& ...ops)
  {
    emplace_all(std::forward<OpTs>(ops)...);
  }

  template <typename OpT>
  requires detail::operation<OpT>
  void push(OpT&& op)
  {
    op_t wrapped {std::forward<OpT>(op)};
    push_erased(std::move(wrapped));
  }

  template <typename OpT>
  requires detail::operation<OpT>
  void emplace(OpT&& op)
  {
    push(std::forward<OpT>(op));
  }

  template <typename OpT, typename ...OpTs>
  requires (detail::operation<OpT> && (detail::operation<OpTs> && ...))
  void emplace(OpT&& op, OpTs&& ...ops)
  {
    emplace(std::forward<OpT>(op)), (emplace(std::forward<OpTs>(ops)), ...);
  }

  template <typename ...OpTs>
  requires (sizeof...(OpTs) > 0 && (detail::operation<OpTs> && ...))
  void emplace_all(OpTs&& ...ops)
  {
    emplace(std::forward<OpTs>(ops)...);
  }

  template <std::ranges::input_range OpsT>
  requires (!detail::tuple_like<OpsT> &&
            detail::operation<std::ranges::range_reference_t<OpsT>>)
  void append(OpsT&& ops)
  {
    for (auto&& op : ops) {
      emplace(std::forward<decltype(op)>(op));
    }
  }

  template <detail::tuple_like TupleT>
  void append(TupleT&& ops)
  {
    std::apply([this](auto&& ...op) {
      (emplace(std::forward<decltype(op)>(op)), ...);
    }, std::forward<TupleT>(ops));
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return ops.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept
  {
    return std::size(ops);
  }

  [[nodiscard]] progress operator()(ceph::libfdb::database_handle dbh,
                                    const limits limits = {})
  {
    require_limits(limits);

    progress result {
      .remaining_ops = std::size(ops),
      .reason = ops.empty() ? halt_reason::completed : halt_reason::limit,
    };

    if (ops.empty()) {
      return result;
    }

    require_database(dbh);

    const auto opts = transaction_options_for(limits);

    while (!ops.empty() && !transaction_limit_reached(result, limits)) {
      auto txn = ceph::libfdb::make_transaction(dbh, opts);

      for (;;) {
        ++result.attempts;

        try {
          // Operations may be replayed until their transaction commits.
          std::invoke(ops.front(), txn);
        } catch (const ceph::libfdb::libfdb_exception& e) {
          if (!e.retryable()) {
            throw;
          }

          // Some day, we won't have to reach into detail-- but that's not today:
          ceph::libfdb::detail::retry_after_error(txn, e.fdb_error_value);

          if (!account_retry(result, limits, e.fdb_error_value)) {
            return result;
          }

          continue;
        }

        // Commit replay status needs a public libfdb wrapper:
        const auto r = ceph::libfdb::detail::do_commit(txn);
        if (0 == r) {
          ops.pop_front();

          ++result.committed_ops, ++result.committed_transactions;

          result.remaining_ops = std::size(ops);

          result.reason = ops.empty() ? halt_reason::completed : halt_reason::limit;
          break;
        }

        // Replaying after commit failure should not stay in detail:
        ceph::libfdb::detail::retry_after_error(txn, r);

        if (!account_retry(result, limits, r)) {
          return result;
        }
      }
    }

    return result;
  }
};

// Compose several transaction operations into one commit boundary:
template <typename ...OpTs>
requires (sizeof...(OpTs) > 0 && (detail::transaction_operation<OpTs> && ...))
[[nodiscard]] constexpr auto transaction(OpTs&& ...ops)
{
  return [ops = std::tuple<std::decay_t<OpTs>...> {
            std::forward<OpTs>(ops)...
          }](detail::txn_t& txn) mutable {
    std::apply([&txn](auto& ...op) {
      (std::invoke(op, txn), ...);
    }, ops);
  };
}

} // namespace rgw::d4n::batch

#endif // WITH_RADOSGW_FDB

#endif // CEPH_RGW_DRIVER_D4N_BATCH_H
