# Datum

**A Common Language for Ceph Data**

*Data semantics, independent of representation*

A guide for Ceph component authors

Jesse F. Williamson <jfw@ibm.com> (assisted-by Codex (GPT-6)) · 25 October 2026

## Overview of Architecture and Behavior

Datum is a C++ library for selecting, observing, and moving data without tying those
operations to one in-memory representation. A component can select key ranges, clip
extents, traverse rows, or encode chosen fields while keeping its own records,
containers, and buffers. The mission is to share the data machinery that Ceph components
repeatedly need, while leaving their domain meaning and input/output decisions with
them.

Consider a request to RADOS (Reliable Autonomic Distributed Object Store) from RGW
(RADOS Gateway). Its arguments already exist in the caller. The client can encode those
borrowed fields directly; the receiver can decode into its own owning record. They share
a description of the message rather than an owning C++ message type. For an OLH (Object
Logical Head) read, the binding can validate a stored log and project selected encoded
events into the reply instead of reconstructing a complete event map. Both paths are
implemented.

### Built from working Ceph code

Datum grew out of libfdb’s generic predicate work, with its tested selection and scan
machinery. Libfdb is Ceph’s FoundationDB integration library. Datum now supports a
**61-operation RGW CLS binding**; SALAD (Storage Abstraction Layer Algebraic Data) uses
it directly for keyspaces, listing plans, record schemas, metadata, and diagnostics.
These are working integrations with distinct data and backend requirements.

> **Less work in measured RGW paths**
>
> The CLS integration uses 60.23% less CPU (central processing unit) time per read in a
> replay of 265 server-produced OLH states. A large synthetic log shows 96.34% less CPU;
> separate allocation measurements fall from 16,033 requests to 21. The graphs ahead
> explain the workloads and the mechanisms behind these results.

### Engineering that can be examined

The current default library configuration passes 128 test cases, including algebraic
checks, finite-domain model checking, ownership, and failure paths; all 17 headers also
compile independently. The CLS integration has separately recorded normal and sanitizer
suites, live RGW checks, and mixed-peer compatibility tests.

The significance for Ceph is a reusable way to express these operations across component
boundaries: existing data structures can cooperate without each boundary demanding a
conversion or a fresh implementation of the same rules.

## Architecture: a small, composable foundation

Datum separates *what an operation needs to observe* from *how a component stores it*.
An interval needs ordered endpoints; a field encoder needs selected members; a scan
needs a row and its position. None of those requires adopting the component’s entire
storage model.

![Pure data foundation above reflected data and observation, with diagnostic rendering
and wire representation below. Arrows point upward to supporting
capabilities.](images/layers.svg)

*Figure 1. Capabilities compose over an ordered-domain foundation. Bindings connect them
to native Ceph data and operations.*

### What is separated, and why

**Descriptions** express selected ranges, record fields, and query requirements.
**Observations** expose those facts from existing data. **Owners** preserve the storage
behind borrowed observations. **Bindings** connect these mechanisms to codecs, buffers,
and backend work, and enforce the component’s semantic rules.

The CLS client, for example, can encode a list that the module receives as a vector.
SALAD can describe a listing’s selection and policy before a backend adapter executes
it. The shared description survives these representation choices; transaction,
admission, and commit decisions stay with their consumer.

Datum is header-only C++26 code. Its ordered-domain algorithms, reflected record
facilities, and concrete adapters are composable capabilities. A task can use selection
geometry without needing a wire codec, or observe a record without changing its owning
type. This separation is what permits the processing choices in the following examples.

The `datum::core` target needs C++26 language and library support, including
`std::inplace_vector`, but no reflection or native Ceph headers. `datum::reflection`
adds reflection for records and diagnostics; codec and formatting adapters add their own
dependencies. Ceph registers the core and reflected test groups only when their respective
capabilities are available. Other Ceph targets keep their existing language standard.
An explicitly requested standalone test build reports unsupported capabilities as errors.

## Why the separation pays off

The RGW CLS integration shows the practical cost of intermediate representations. Its
OLH path validates a canonical stored log and projects selected encoded events into a
reply, avoiding a fully materialized event map. Datum supplies shared field
descriptions; RGW retains its codecs, record rules, and ownership decisions.

![CPU per OLH read relative to native RGW on main, for captured server states and three
synthetic log sizes. Bars include 95% intervals.](images/olh-cpu.svg)

*Figure 2. CPU per OLH read compared with native RGW on main, normalized to 100. Lower
bars mean less CPU; whiskers show 95% intervals. The synthetic logs use one physical
input buffer.*

Replaying 265 server-produced states uses **60.23% less CPU** per read. The synthetic
1,000-epoch log, with two events per epoch and 128-byte text fields, uses **96.34% less
CPU** with its input in one buffer.

The architectural benefit is freedom to process the representation the task needs, while
preserving a clear contract at the boundary. The OLH and allocation figures describe the
measured native integration snapshots used for this guide. They measure local processing;
application throughput also depends on storage and RPC (remote procedure call) overhead.

## Less temporary work, independent representations

Borrowed request builders remove an owning record whose only job was encoding. The
complete builder with 1,000 removals drops from **3,025 allocation requests to 13**; the
OLH fixture drops from **16,033 to 21**. The graph also includes GC (garbage collection)
request building.

![Allocation requests for native RGW and Datum complete, GC, user, and OLH paths on a
logarithmic scale.](images/allocation-requests.svg)

*Figure 3. Allocation requests per action compared with native RGW on main, on a
logarithmic axis. These separate allocation measurements use the native OLH fixture,
rather than the coalesced input in the CPU comparison.*

**Native prepare builder**

``` cpp
rgw_cls_obj_prepare_op call;
call.op = op;
call.tag = tag;
call.key = key;
call.locator = locator;
bufferlist in;
encode(call, in);
o.exec(method::bucket_prepare_op, in);
```

**Datum binding**

``` cpp
auto in = write_datum::encode_prepare(op, tag, key, locator);
o.exec(method::bucket_prepare_op, in);
```

The arguments remain valid through synchronous encoding; the resulting message owns its
bytes. The receiver chooses its own owning record.

## Why this matters to Ceph component authors

**Reuse existing data and algorithms.** Logic coupled to one record or container forces
another component to convert its data or reproduce the logic. Datum’s observations let
common operations work with native forms. Libfdb supplied the selection foundation; its
existing API (application programming interface) illustrates the idea by subtracting
internal keys from a visible prefix before scanning it:

``` cpp
const auto visible_records =
  q::difference(q::prefix("collection/records/"),
                q::prefix("collection/records/.internal/"));

for (const auto& [key, value] : lfdb::scan(dbh, visible_records)) {
  process(key, value);
}
```

Here `lfdb` names `ceph::libfdb` and `q` its query namespace. Libfdb’s extensive tests
cover algebraic laws and selector boundaries, along with FoundationDB queries,
forward/reverse scans, and pagination. Datum carries the generic predicate work forward
into a reusable component.

**Avoid unnecessary movement and reconstruction.** Copying arguments into an owning
request merely to encode them, or decoding a whole record to inspect a few fields, can
dominate small operations. The CLS examples show existing code doing less of this work.
A shared field profile also lets storage encoding and diagnostic output evolve
independently.

**Give subtle boundary rules one clear expression.** Selection must preserve which
source supplied the bytes. A borrowed view must keep usable storage. A scan stopping
before a row must resume at that row. Datum makes these distinctions part of the
reusable machinery, so a component can focus on its domain rules.

### Why the scope can extend beyond one RGW path

RGW’s SAL (Storage Abstraction Layer) brings together listings, reads, and
backend-specific representations; SALAD already uses Datum beneath its domain policy.
RBD (RADOS Block Device) and ObjectStore extent processing also combine range geometry
with native source data. CephFS (Ceph File System) has metadata records; lifecycle and
multisite work have paged traversal and continuation rules. These are meaningful places
to evaluate the same capabilities, because the underlying problems recur.

The opportunity is to share well-defined operations while preserving each component’s
data layout and authority. The existing CLS and SALAD work shows that this separation is
practical; its value in another path depends on the work that path can avoid or reuse.

## Select data without losing its identity

An extent operation needs more than a set of covered coordinates: it needs the data
associated with those coordinates. Datum separates those two questions. Its interval
algebra works over an ordered domain. Union, intersection, and difference describe
selected points without requiring arithmetic on them. Here `iv` names `datum::interval`:

``` cpp
struct positions_domain final : iv::comparison_domain<unsigned> {};
using Q = iv::query<positions_domain>;

const auto selected = iv::difference(Q::closed_open(0, 100), Q::singleton(40));
const auto joined = iv::set_union(Q::closed_open(0, 10), Q::closed_open(20, 30));
const auto overlap = iv::intersection(joined, Q::closed_open(5, 25));
```

Their canonical coverage consists of ordered, nonempty, coalesced intervals with
explicit endpoint inclusion. The same algebra applies to keys and byte positions because
it depends on order. A consumer can traverse a selection or materialize its coverage.

`comparison_domain<T, Compare>` shares a strong comparison policy; a named derived domain
keeps byte offsets, versions, or other coordinate systems distinct even when they use the
same value type. Points need no default constructor, assignment, equality operators, or
arithmetic. A supplied comparator can order points without native comparison operators.

A prefix profile additionally supplies `prefix_upper_bound()`, the optional exclusive end
of every key sharing a prefix. For byte strings, `"ab"` ends at `"ac"`; this is independent
of the next individual key. An absent upper bound extends the range to positive infinity.
The byte-string profile recognizes an empty prefix as universal. Ordinary open and closed
intervals require ordering alone.

### Coverage alone does not identify the data

A union answers which coordinates are selected. To use their payloads, an extent
consumer also needs to know which source record supplied each piece. `clip_view`
preserves that association while intersecting source geometry with the selection.

![Source extents A, B, and C intersect requested half-open ranges from 5 to 18 and from
34 to 40. The resulting cuts retain their source identities, including the touching A
and B cuts.](images/clipping.svg)

*Figure 4. The selected portions of A and B remain distinct even though their combined
coverage is contiguous. Each cut still identifies its source.*

The result is

`(A,[5,12)), (B,[12,18)), (C,[34,40)).`

A source may carry a buffer, object identity, or checksum context; Datum need not know
those native fields to preserve their association. Borrowed cuts remain tied to their
source and traversal lifetimes.

Order determines coverage. Measuring covered bytes or applying a byte quota additionally
needs a metric; holes consume no quota. `measure()` requires distance, zero, and addition.
`take(coverage, quota, metric)` instead needs ordered quantities, subtraction, and point
advancement to select a finite, half-open coverage prefix. A measurement-only provider need
not supply those operations. Storage alignment and coordinate conversion remain with the
binding.

## Borrow data without losing its lifetime

Avoiding copies is useful only while the borrowed data remains usable. Keeping interval
endpoints does not keep a source record or its payload alive. Datum makes that storage
relationship explicit, so an adapter can retain the required owner rather than
flattening or copying data just to simplify lifetime.

![Endpoint retention borrows the source record. Payload retention stores an owner and
recomputes its projection through a lens, including after a move.](images/lifetime.svg)

*Figure 5. Endpoint values and payload storage have different ownership needs.*

A `retained_view` stores an owner and a lens. The projection is recomputed from the
owner after a move, avoiding a cached pointer into the old wrapper:

``` cpp
datum::retained_view retained {std::string {"abcdef"}, datum::byte_slice {1, 3}};
const auto moved = std::move(retained);
const auto span = moved.get(); // projection is recomputed from the moved owner
assert(std::string_view(std::data(span), std::size(span)) == "bcd");
```

### Observe fragments without imposing a flat payload

`byte_cursor` exposes bounded slices across a stable fragment range; `write_fragments()`
delivers those slices to a sink:

``` cpp
const std::array source {'a', 'b', 'c', 'd', 'e', 'f'};
const std::array fragments {
    std::span {source}.first(2), std::span {source}.subspan(2)};
datum::byte_cursor input {fragments};
const auto slice = input.take(4);
std::string text;
datum::write_fragments(slice, [&](auto block) {
    text.append(std::data(block), std::size(block));
});
```

These operations preserve fragmented observation. Cursor reads preflight stable,
multipass fragments; slices keep independent positions when the cursor advances.
`write_fragments()` also accepts single-pass input ranges and borrows its synchronous sink,
including a mutable callable that cannot be copied or moved. Only writing into fragments
requires mutable traversal. The binding chooses whether to copy or retain payloads and
adapts any codec that requires contiguous input. Ownership preserves backing lifetime;
application admission and mutation policy remain separate.

## Share a contract, keep native data structures

A wire contract needs agreement on fields and their meaning. It does not require the
client and receiver to use the same C++ record or container. The CLS binding uses
Datum’s field profiles with RGW framing and leaf codecs to make that distinction
concrete.

![Independent client and module records communicate through a shared wire profile and an
explicit representation contract.](images/cls-binding.svg)

*Figure 6. Independent client and module data structures share an explicit
representation contract.*

A complete request borrows the client’s removal list and decodes into the module’s
vector. BI (bucket-index) replies can fill a list or vector directly. This BI request
encoder selects the prefix fields for its frame:

``` cpp
using prefix = detail::request_prefix;

archive.frame(
    datum::binary_fields<&prefix::maximum, &prefix::filter,
                         &prefix::marker, &prefix::reshard> {},
    Version,
    prefix {maximum, filter, marker, static_cast<std::uint8_t>(reshard)});
```

### Select a wire schema directly

For an ordinary record in declaration order, start with `binary_schema_t<Record>`.
An explicit nonempty `binary_fields` selection infers its record from the member pointers
and chooses a different order or an intentional subset:

``` cpp
struct reply_record final
{
    std::uint64_t key;
    std::string value;
    unsigned local_state = 7;
};

using reply_wire = datum::binary_fields<&reply_record::value, &reply_record::key>;
const reply_record original {42, "payload", 99};
const auto bytes = datum::render::to_bytes<reply_wire>(original);
const auto restored = datum::render::from_bytes<reply_wire>(bytes);
assert(42 == restored.key && "payload" == restored.value && 7 == restored.local_state);
```

A fresh decode uses ordinary initialization for omitted fields; decoding into an existing
record preserves its omitted fields. Selected schema members must be distinct, direct,
and public. Native field profiles retain their broader observation and framing uses; a
profile alone does not grant ownership or permission to construct a decoded record.
Datum’s diagnostic profiles select fields, labels, and redaction independently of wire
profiles. A storage layout need not dictate what a diagnostic reveals.

The optional zpp codec preserves `std::system_error` and its error code. A complete-message
decode rejects trailing bytes with `std::errc::bad_message`; an outer input-byte limit
still throws `std::length_error`. Existing `std::runtime_error` catches also catch
`std::system_error`. A binding chooses how to expose an error to its caller.

### Make the publication boundary deliberate

The BI client decodes into a candidate before updating caller-visible state:

``` cpp
  auto [candidate, is_truncated] = bi_datum::decode_reply<EntriesT>(out);
  destination->swap(candidate);
  *truncated = is_truncated;
```

BI and OLH bindings preserve caller results when decoding fails; their chosen
destinations support nonthrowing publication after a complete decode. The binding checks
enclosing bounds and retains payload owners before using borrowed fields. This is a
binding guarantee built with the shared mechanisms; individual low-level reads and
writes can have partial effects.

Tests exercise proper reply truncations, fragmented inputs, retained owners, and native
wire compatibility. The engineering benefit is both freedom of representation and a
boundary whose behavior can be tested directly.

## Make scan progress precise

A listing or paged query can lose a row if its continuation marker advances past data
the consumer has not consumed. Offering, consuming, and accepting a row are different
events; they determine where an ordered scan resumes. Datum captures an owned position
before the consumer can move the row, then derives the resume boundary from the
consumer’s decision. A custom profile supplies `position(row)` and
`next(position, consumed)`, keeping backend-specific position state outside the algorithm.

![Capture an owned position before offering a row. accept_and_stop resumes after the
row; reject_and_stop resumes at the row.](images/resume.svg)

*Figure 7. Resume after a consumed row; resume at a row left unconsumed.*

Here `tr` names `datum::traversal`. The consumer skips 1, accepts 2, and stops before
consuming 3; the result resumes inclusively at 3:

``` cpp
void scans()
{
    const std::array rows {1U, 2U, 3U, 4U};
    const auto consumer = [](unsigned value) {
        if (1 == value) {
            return datum::acceptance::skip;
        }
        if (3 == value) {
            return datum::acceptance::reject_and_stop;
        }

        return datum::acceptance::accept;
    };
    const auto plain = datum::visit_range(rows, consumer);
    assert(3 == plain.offered && 2 == plain.consumed && 1 == plain.accepted);
    const auto scan = tr::scan_range(
        rows, consumer, tr::ordered_positions {[](unsigned value) {
            return iv::boundary<positions_domain>::closed(value);
        }});
    assert(scan.resume && scan.resume->key() == 3 && scan.resume->inclusive());
}
```

Page traversal similarly leaves the backend’s continuation state explicit. A resume
position describes progress; the backend still supplies ordering, snapshot, and
transaction semantics.

## Admit work before doing it

Resource limits need a deliberate decision before the work they protect. A
`resource_budget` tracks cumulative input bytes, payload bytes, elements, requested
allocation bytes, and work; a scoped depth guard releases nesting depth on exit.
The codec or allocator must cooperate in charging the work: these counters do not
observe arbitrary allocations.

``` cpp
datum::resource_budget budget {{.elements = 2}};
assert(budget.try_charge(datum::resource::elements, 2));
assert(!budget.try_charge(datum::resource::elements, 1));
assert(2 == budget.used(datum::resource::elements));
```

`try_charge()` applies the charge on success and preserves usage on refusal, including
representability overflow. Use this Boolean result for ordinary admission. `charge()`
uses the same checks and throws `std::length_error` on refusal, so a strict allocation
guard cannot accidentally proceed. Invalid resource identifiers remain argument errors.
An `unlimited_budget` supplies the same operations without accounting. None of these
choices establishes semantic validity, a transaction, or permission to publish a result.

### Measured cost of handled outcomes

An isolated GCC 17 microbenchmark on 25 October compared the previous throw/catch
admission and codec error translation with the new paths. Twelve balanced paired runs
per case measured CPU time with `-O2`; the budget caller used C++23 and the codec caller
used C++26 with reflection.

| Operation | Previous ns/call | Updated ns/call | CPU change |
| --- | ---: | ---: | ---: |
| Refused ordinary admission | 371.46 | 0.59 | −99.84% |
| Rejected codec input | 888.80 | 458.27 | −48.44% |
| Refused strict charge | 371.22 | 399.64 | +7.66% |

Ordinary refusal avoids throwing; codec rejection avoids translating and throwing the
same failure again. Strict refusal still throws. Its initial slowdown was sensitive to
the surrounding caller: a reduced caller measured −0.26%, with a 95% interval of
[−0.60%, +0.07%]. That follow-up does not erase the initial slowdown or establish parity.
The tiny codec-success loop measured 0.3820 versus 0.3822 ns. These are mechanism
measurements; no production resource-budget consumer was measured, and they do not
establish RGW throughput or combine with the earlier OLH results.

## Keep domain meaning with the component

SALAD uses Datum directly for ordered selections, keys, typed plans, and record
profiles. SALAD supplies the bucket, namespace, version, and delimiter policy; its
backend adapters turn descriptions into native work. This lets the shared facilities
serve a domain model without absorbing it.

![SALAD domain records and policy compose Datum capabilities into plans, schemas, and
validated records. Backend adapters supply native buffers, selections, and
transactions.](images/salad.svg)

*Figure 8. SALAD composes data capabilities; domain policy and backend execution remain
explicit parts of the component.*

Its listing plan exposes interval geometry while retaining the rest of the operation’s
policy:

``` cpp
const auto plan = operation::plan_object_listing(query);
const auto intervals = datum::interval::compile_intervals<1>(operation::scan(plan));
```

### A decoded record still needs its domain’s meaning

SALAD serializes canonical metadata records with Datum. On reading a listing row, it
decodes the value and checks that the record belongs at the storage key:

``` cpp
inline std::optional<object_version_listing_scan_entry>
object_listing_scan_entry_from_kv(const object_listing_kv_row& row)
{
  auto record = datum::render::from_bytes<data::object_version_record>(row.value);

  if (!matches_key(record, row.storage_key)) {
    return std::nullopt;
  }

  return object_version_listing_scan_entry {
    .storage_key = row.storage_key,
    .version = std::move(record)
  };
}
```

Datum supplies record representation; SALAD supplies the key/record identity rule. The
same architectural boundary runs through the earlier examples: select and observe
existing data, preserve what makes it usable, and keep semantic decisions with the
component that understands them. This gives Ceph authors a common language for data
while keeping domain rules and storage execution under each component’s control.

## Conceptual index

These definitions explain how specific terminology is used in Datum.

**Binding.** An adapter between Datum and Ceph’s native types and backend operations.
Client and module bindings can use different C++ types with the same wire contract. The
binding controls when completed results become visible.

**Borrowing, ownership, and retention.** A borrow accesses storage kept alive elsewhere,
like a C++ reference. An owner controls the required lifetime. Retention keeps that
owner alive so a view remains usable; it need not copy payload bytes.

**Canonical form and admission.** Canonical intervals are sorted, nonempty, and
coalesced wherever their union is one interval. Admission checks the invariants required
by an algorithm. A component’s semantic validity checks remain distinct from
successfully decoding its bytes.

**Codec and wire profile.** A codec translates fields to or from bytes. A wire profile
selects their order, framing, and versions. Matching profiles let peers use independent
records in memory.

**Composition and purity.** Composition connects small operations, such as intersection
followed by traversal. A pure observer produces the same observation for unchanged
inputs without mutating them or hidden state.

**Concept and constraint.** A named compile-time requirement on a type’s supported
operations, written with `concept` and `requires`. Think of checked template
requirements: inheritance and virtual dispatch are unnecessary.

**Cursor and range.** A range exposes a sequence through iterators; a cursor tracks a
position while reading. Datum’s byte cursor can read across fragments without requiring
one flattened buffer.

**Field profile (schema).** A selected, ordered set of record fields used by generic
observation or construction. Wire schemas specify approved field order; diagnostic
schemas separately select what may be displayed. A nonempty `binary_fields` selection
also identifies the record for direct codec use when its members satisfy schema rules.

**Lens.** A small callable that selects a field or a derived observation: think of an
accessor object. `member_lens<&Row::key>{}(row)` reads `row.key`. C++26 reflection maps
that same member lens onto generated record views, preserving their selected reference
or view types.

**Materialization and encoded projection.** Materialization builds an owning
representation, such as a decoded event map. Encoded projection validates and selects
data in its encoded form, avoiding that map while still constructing the required reply
framing.

**Observation and view.** Observation reads what an operation needs. A view exposes data
through references or other small projections. `view_of<Record>` defaults to const
references; the source storage must outlive its borrows.

**Ordered domain and selection.** A domain supplies point values and their ordering:
keys or byte positions, for example. A selection describes which points to keep.
Interval union, intersection, and difference need ordering; measuring distance
additionally needs a metric. Prefix bounds describe all points sharing a prefix,
independently of a next-point operation.

**Resource budget.** Explicit accounting charged before cooperating operations perform
work. Ordinary refusal is a Boolean result; strict guards throw on refusal. Cumulative
requested allocation bytes are distinct from live memory use.

**Reflection and constant evaluation.** C++26 reflection exposes type and member
information during compilation. Datum uses it to generate named views and field
metadata. Calls to a `consteval` function are evaluated at compile time; a `constexpr`
function can support both compile-time and runtime calls.
