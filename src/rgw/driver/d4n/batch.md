# D4N Batch transactions

A `batch::plan` is a FIFO list of transactions that D4N owns until they have
finished. Each transaction contains one or more directory mutations. Successful
transactions are removed; stopped transactions remain at the front for D4N to
retry, inspect, or reconcile.

Include the adapter for the selected directory backend:

```cpp
#include <utility>

#include "driver/d4n/batch_fdb.h"

namespace batch = rgw::d4n::batch;

auto transact = batch::make_transactor(fdb_database);
```

For Redis, include `batch_redis.h` and pass a `RedisPool` instead. The plan and
result handling are otherwise the same.

## Update one directory entry

Assume that `block_key` and `encoded_block` were produced by D4N's directory
encoding. A single mutation is also a single-transaction plan:

```cpp
batch::plan work {
  batch::put {
    .key = block_key,
    .value = encoded_block
  }
};

const auto result = transact(work, {.retries = 3}, y);
if (batch::halt_reason::completed != result.reason) {
  // Keep work: its unfinished transaction is still at the front.
  return;
}
```

Completion, rather than `last_error`, is the success test. A transient error may
remain in `last_error` after a retry succeeds.

## Put separate transactions in one batch

The plan is the batch. Construct transaction specifications when several
mutations must succeed together, then add those transactions to the plan:

```cpp
auto publish_block = batch::transaction(
    batch::put {
      .key = object_directory_key,
      .value = encoded_object_metadata
    },
    batch::put {
      .key = block_directory_key,
      .value = encoded_block_metadata
    });

auto retire_old_block = batch::transaction(
    batch::erase {.key = old_block_directory_key},
    batch::put {
      .key = eviction_record_key,
      .value = encoded_eviction_record
    });

batch::plan work {std::move(publish_block)};
work.emplace(std::move(retire_old_block));

const auto result = transact(work, {}, y);
```

The two plan entries run in order, but are not atomic with one another. If the
first commits and the second stops, only the second remains in `work`. On Redis,
the keys within each transaction must use the same cluster hash tag, such as
`{bucket-id}`; the two separate transactions need not share a slot.

## Handle a stopped batch

Backend failures are returned in `progress`. A stopped plan must remain owned by
D4N; do not discard it or call `acknowledge_front()` merely because retries were
exhausted.

```cpp
const auto result = transact(work, {.retries = 3}, y);
if (batch::halt_reason::completed != result.reason) {
  const auto& error = result.last_error;
  if (error) {
    ldpp_dout(dpp, 0)
        << "D4N directory batch stopped: "
        << error->code.category().name() << ':' << error->code.value()
        << ": " << error->diagnostic << dendl;
  }
}

switch (result.reason) {
  case batch::halt_reason::completed:
    return;

  case batch::halt_reason::transaction_limit:
    queue_for_later(std::move(work));
    return;

  case batch::halt_reason::retry_limit:
    // Nothing applied on the final attempt; a later retry remains safe.
    queue_for_later(std::move(work));
    return;

  case batch::halt_reason::failed:
    // A permanent failure definitely did not apply this transaction.
    retain_for_diagnosis(std::move(work));
    return;

  case batch::halt_reason::unknown_result:
  case batch::halt_reason::partial_result:
    // Do not replay until the directory state has been checked or repaired.
    queue_for_reconciliation(std::move(work));
    return;
}
```

The queue functions above represent D4N policy, not Batch API. After
reconciliation proves that the front transaction is complete,
`work.acknowledge_front()` may advance it. Invalid configuration or a violated
API contract may be reported by exception rather than `progress`; add context
at the request or service boundary rather than treating it as a retryable
backend error.

## Hide the usual log-and-leave policy

Most call sites only need to log a stop and leave. Put that policy in one small
helper, but preserve both the plan and its result in D4N's recovery queue:

```cpp
template <typename Transactor>
[[nodiscard]] bool
run_directory_batch(const DoutPrefixProvider* dpp,
                    Transactor& transact,
                    batch::plan& work,
                    optional_yield y,
                    D4NBatchRecovery& recovery)
{
  auto result = transact(work, {.retries = 3}, y);
  if (batch::halt_reason::completed == result.reason) {
    return true;
  }

  if (const auto& error = result.last_error) {
    ldpp_dout(dpp, 0)
        << "D4N directory batch stopped with "
        << error->code.category().name() << ':' << error->code.value()
        << ": " << error->diagnostic << dendl;
  } else {
    ldpp_dout(dpp, 0) << "D4N directory batch stopped" << dendl;
  }

  recovery.defer(std::move(work), std::move(result));
  return false;
}
```

The call site remains explicit about control flow:

```cpp
if (!run_directory_batch(dpp, transact, work, y, directory_recovery)) {
  return;
}

// All directory updates in work were acknowledged.
```

`D4NBatchRecovery` is an application abstraction: `defer()` retains the plan and
its `progress`, scheduling ordinary retry or diagnosis for definite outcomes
and reconciliation for `unknown_result` or `partial_result`. The helper neither
replays ambiguous work nor advances it.

## Backend boundaries

- FoundationDB applies every mutation in one plan entry atomically.
- Redis `MULTI`/`EXEC` prevents interleaving, but does not roll back commands
  that succeeded before another command failed at runtime.
- A lost commit response can be `unknown_result` even when the database did
  commit. Stop and reconcile; do not infer failure from the missing response.
- By default, automatic retry is limited to definitely-unapplied transient
  failures.
  `retry_for_convergence` also permits replay after an unknown result, but only
  when replay of the entire transaction is safe.
- `batch_fdb_native.h` supplies an FDB-specific plan for reads, conflict ranges,
  atomic mutations, and other operations outside the portable `put`/`erase`
  contract.
