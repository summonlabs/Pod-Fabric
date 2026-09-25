# Pod Fabric

Pod Fabric is a generation-bound runtime for a **pod**: a governed group of
racks whose combined fabric state has to be decided, published and fenced as a
single unit.

It answers one question, continuously and deterministically:

> Given authoritative rack states, inter-rack links, routing and capacity
> evidence, protected obligations, failure domains and exact generations, what
> pod-level state is authoritative *now*, which rack-to-rack connectivity may be
> used, and what must be fenced or degraded when a member changes?

Pod Fabric composes the outputs of a Rack Network Fabric. It does not own
rack-internal truth.

---

## What this repository contains

| Component | Path | What it is |
| --- | --- | --- |
| Core types | `include/podfabric/core` | Typed outcomes, canonical identities, SHA-256/CRC-32C, checked arithmetic, bounded codecs, clocks |
| Model | `include/podfabric/model` | Generations, epochs, incarnations, members, links, routes, failure domains, obligations, capacity claims, policy, pod state |
| Composition engine | `include/podfabric/engine` | The deterministic composer, the double-count-free capacity ledger, dependency indexing and selective revalidation, authority token validation |
| Codecs | `include/podfabric/codec` | Canonical binary encoding and a strict, bounded text exchange format |
| Durability | `include/podfabric/persist` | Versioned checkpoint plus write-ahead journal with checksums, torn-tail repair and conservative replay |
| Transport | `include/podfabric/transport` | Checksummed framing and blocking loopback TCP with explicit deadlines |
| Control | `include/podfabric/control` | The pod controller, the wire protocol, the bounded connection pool and a client |
| Tools | `apps` | `podfabd` (daemon) and `podfabricctl` (inspection and control) |
| Tests | `tests` | Unit, property, adversarial, concurrency and multiprocess suites |
| Examples | `examples` | Composition and selective-revalidation examples, plus an independent find_package consumer |
| Benchmarks | `benchmarks` | Completed-work composition benchmark |

---

## Adjacent-runtime boundaries

Pod Fabric is one runtime in a stack. These boundaries are explicit and are
enforced by the type system, not by convention.

**Consumed, never owned**

- **Rack Network Fabric.** Pod Fabric receives a rack descriptor's exact
  generation, its content digest and its declared schema revision. It verifies
  compatibility; it never derives rack-internal truth. A rack descriptor whose
  schema revision is unknown is reported as UNSUPPORTED and the member is not
  adopted.
- **Routing.** Route and path authority arrive as opaque references with their
  own generation and digest, plus the ordered rack path and the generations the
  path was installed against. Pod Fabric never computes a route. A route that is
  withdrawn or not settled is reported as BLOCKED or INDETERMINATE.
- **Telemetry.** No low-level telemetry is consumed. The only health input is
  the coarse health verdict that the owning subsystem publishes.

**Out of scope entirely**

- Cluster-wide routing decisions, optical or layer-0 control, and site
  federation. None of these are modelled here, and nothing in this repository
  claims to implement them.

**Not implemented, and marked UNSUPPORTED rather than approximated**

- No RDMA, InfiniBand, NVLink, switch/ASIC or vendor SDK integration exists in
  this repository. There is no code here that talks to a NIC, a switch or an
  optical element.
- No physical networking effect is measured or claimed. Capacity figures are
  *claims* supplied as evidence by the owning subsystem.
- No multi-host operation is claimed. The only real transport in the tree is a
  loopback TCP listener.

**Evidence quality.** Every record carries provenance that distinguishes REAL
from SYNTHETIC from UNKNOWN. Everything the built-in generator produces is
marked SYNTHETIC and describes no real hardware.

---

## Guarantees

These are the properties the test suite demonstrates. Each has at least one
dedicated case; the multiprocess suite proves the durability ones across
independent operating-system processes.

1. **A stale rack generation cannot sustain pod authority.** Evidence whose
   generation is behind the recorded generation is STALE, its capacity and links
   are withheld, and the pod's authority code is at least STALE.
2. **A conflicting generation is never resolved by order.** Two records that
   claim the same rack, or one generation reported with two different descriptor
   digests, become CONFLICTING. No silent winner is chosen.
3. **One rack reincarnation invalidates only its dependents.** The dependency
   index reports exactly which decisions must be recomputed, and the runtime
   verifies that no decision outside that set changed.
4. **Capacity is never double-counted.** Claims are keyed by the physical
   resource they describe. Two owners reporting the same physical budget are
   counted once; disagreeing magnitudes become CONFLICTING and take the smaller
   value.
5. **Failure domains are honoured.** A rack in two domains of the same kind is a
   conflict, diversity is decided by an explicitly deterministic packing of
   rack-disjoint domains, and a link that shares fate with an unhealthy domain
   cannot be reported better than that domain.
6. **Restart never resurrects stale authority.** A new process incarnation
   advances the pod epoch, fences every lease minted by the previous
   incarnation, and reports RECOVERING rather than ACTIVE on its first round.
7. **A retired pod stays retired.** RETIRED is terminal; authority is REFUSED
   for good.
8. **Missing evidence is never turned into success.** Absent health, absent
   connectivity evidence, an undeclared failure domain or an unreadable durable
   record all degrade the answer rather than disappearing into it.

---

## Lifecycle

```
constructing ---> active <---> degraded
     |              |  |          |
     |              |  +----------+---> partitioned ---> recovering ---+
     |              |                        ^                         |
     |              +---> maintenance -------+                         |
     |              +---> draining ----------+                         |
     +-----------------------------------------------------------------+

any ---> retired (terminal)
```

A partition never heals straight back to ACTIVE: it reports RECOVERING for one
round first, so a pod cannot claim to be healthy in the same round in which it
regained connectivity.

---

## Building

Requirements: a C++20 compiler (verified with MSVC 19.44 / Visual Studio 2022
17.14 and CMake 4.3), CMake 3.25 or newer, and a generator. There are no
third-party dependencies; the build neither downloads nor vendors anything.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `PODFABRIC_BUILD_TOOLS` | ON | Build `podfabd` and `podfabricctl` |
| `PODFABRIC_BUILD_EXAMPLES` | ON | Build the examples |
| `PODFABRIC_BUILD_TESTS` | ON | Build and register the test suites |
| `PODFABRIC_BUILD_BENCHMARKS` | ON | Build the composition benchmark |
| `PODFABRIC_WARNINGS_AS_ERRORS` | ON | `/WX` on MSVC, `-Werror` elsewhere |
| `PODFABRIC_ENABLE_ASAN` | OFF | Build with AddressSanitizer |

No test declares a timeout. A hang is a defect to diagnose, not a result to
classify.

---

## Installing and consuming

```
cmake --install build --prefix /some/prefix
```

The install exports a `PodFabric` CMake package. A downstream project needs
only:

```cmake
find_package(PodFabric 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE PodFabric::podfabric)
```

A complete, independent consumer lives in `examples/downstream`. It is a
separate CMake project and is not part of the Pod Fabric build; it is
configured, built and run against an installed prefix only.

---

## Using the runtime

### Compose a pod state

```cpp
podfabric::CompositionRequest request;
request.snapshot = evidence;                 // what the racks reported
request.context.expectations = recorded;     // what the pod already believes
request.context.epoch = podfabric::PodEpoch(4);

const auto state = podfabric::compose(request);
if (!state.ok()) {
  // The request itself was malformed, oversized or of an unknown revision.
  // A conflicted but well-formed request succeeds and reports the conflict.
}
```

Composition is pure: the same request always produces a byte-identical
document, and no ordering of the input collections can change the result.

### Drive the pod

```cpp
podfabric::ControllerOptions options;
options.pod = podfabric::PodId::from_canonical_literal("pod-a");
options.store_directory = "E:/var/lib/pod-a";   // empty means memory-only
podfabric::PodController controller(options);
controller.open();                              // recovers, mints an incarnation
controller.apply(evidence);                     // one evidence round
controller.set_admin_state(rack, podfabric::AdminState::Draining);
controller.retire();                            // terminal
```

The controller is safe for concurrent use. Its lock order is `write_mutex_`
then `state_mutex_`, and **no observer callback is ever invoked while a lock
is held** - which is what lets a callback call straight back into the
controller, including from the same thread.

### Run the daemon

```
podfabd --pod pod-a --store E:/var/lib/pod-a --listen 127.0.0.1:0 --require-token
podfabd pod=pod-a endpoint=127.0.0.1:51422 recovery=FRESH epoch=1 incarnation=...
```

`--listen host:0` asks the operating system for a free port and the chosen
endpoint is printed on the first line, which is what the test harness and the
tooling use to discover it.

### Inspect and control

```
podfabricctl synthesize --out pod.pfs --pod pod-a --racks 8 --domains 4 --seed 7
podfabricctl compose   --snapshot pod.pfs
podfabricctl status    --endpoint 127.0.0.1:51422
podfabricctl describe  --endpoint 127.0.0.1:51422
podfabricctl apply     --endpoint 127.0.0.1:51422 --snapshot pod.pfs
podfabricctl drain     --endpoint 127.0.0.1:51422 --rack rack-3
podfabricctl mint      --endpoint 127.0.0.1:51422 --scope pod --out lease.bin
podfabricctl check     --endpoint 127.0.0.1:51422 --token lease.bin
podfabricctl shutdown  --endpoint 127.0.0.1:51422
podfabricctl selftest
```

`podfabricctl selftest` runs a small in-process end-to-end check without a
daemon and prints one line per assertion.

---

## Durability

A store directory holds three files:

| File | Contents |
| --- | --- |
| `pod.checkpoint` | A complete durable state, written atomically (temporary file, then an atomic replace) and protected by a SHA-256 of its body |
| `pod.journal` | A checksummed append-only log of `Begin`/`Commit` transactions |
| `pod.lock` | An exclusive lock held for the lifetime of the process that owns the store |

Recovery outcomes are reported explicitly:

| Outcome | Meaning |
| --- | --- |
| `FRESH` | No durable state existed |
| `CLEAN` | Every record replayed; the tail was intact |
| `TORN_TAIL_REPAIRED` | An incomplete trailing record was discarded |
| `CORRUPT_TAIL_REPAIRED` | A checksum-failing trailing record was discarded |
| `AMBIGUOUS_COMMIT_ROLLED_BACK` | A `Begin` without a `Commit` was rolled back |
| `UNSUPPORTED_VERSION` | The durable revision is not understood; the store refuses to open |
| `CORRUPT` | Damage beyond conservative recovery; the store refuses to open |
| `REFUSED` | The directory is already locked, or is unusable |

The scan stops at the first damaged record. It never skips over one, so a
mid-file corruption discards the remainder and is reported rather than silently
repaired. A damaged tail is truncated at the last record that passed its
checksum, and the discarded byte count is always reported.

Recovered dynamic evidence is historical. It can fence; it cannot establish.

---

## Tooling output

`podfabricctl describe` and the `Describe` protocol message render the full
pod state document, including one line per derived decision with the exact
dependencies it consumed and a fingerprint of the decision.
`podfabricctl status` prints the summary line:

```
code=OK epoch=4 lifecycle=active established=8 fenced=0 stale=0 conflicting=0
digest=... recovery=CLEAN ambiguous-commit=0 incarnation=...
```

---

## Benchmark

`benchmark_compose` reports wall-clock cost per composition together with the
work actually completed, so the numbers can be checked rather than trusted. The
table below is one run of `benchmark_compose 200` on the machine and build
described in the release notes; treat it as a regression baseline, not as a
claim about any other machine.

---

## Persistence and protocol revisions

Every serialised artefact is versioned and refuses to be misread:

| Artefact | Revision | Behaviour on mismatch |
| --- | --- | --- |
| Composition input snapshot | `podfabric.snapshot/1` | UNSUPPORTED |
| Text exchange document | `podfabric.text/1` | UNSUPPORTED |
| Binary snapshot encoding | 1 | UNSUPPORTED |
| Durable state | `podfabric.durable/1` | UNSUPPORTED, store refuses to open |
| Pod state document | `podfabric.podstate/1` | informational |
| Journal framing | 1 | UNSUPPORTED, store refuses to open |
| Wire frames | 1 | UNSUPPORTED, frame rejected |
| Wire protocol | 1 | UNSUPPORTED, frame rejected |

---

## Testing

```
ctest --test-dir build --output-on-failure
```

| Suite | What it covers |
| --- | --- |
| `core` | Digests, identifiers, checked arithmetic, UTF-8, bounded codecs |
| `model` | Lifecycle legality, policy and snapshot validation, canonical ordering |
| `codec` | Binary and text round trips, malformed, truncated and oversized input |
| `ledger` | Double counting, deduplication, conflicts, overflow, domain breakdowns |
| `engine` | The composition guarantees end to end |
| `dependency` | Dependency indexing, plans, soundness of selective revalidation |
| `authority` | Token fencing: incarnation, epoch, expiry, revocation, scope |
| `persist` | Framing, torn tails, ambiguous commits, version refusal, locking |
| `transport` | Framing, hostile input, loopback round trips, shutdown |
| `control` | Controller semantics, observers, reentrancy, retirement, restart |
| `property` | Seeded invariants, with the failing seed printed on failure |
| `adversarial` | Random bytes, single-byte corruption, extreme values, oversized counts |
| `concurrency` | Concurrent apply, competing updates, readers, observers, start/stop |
| `multiprocess` | Real OS processes over loopback TCP, including hard kill and restart |

### Sanitizers

The whole suite also runs clean under AddressSanitizer:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPODFABRIC_ENABLE_ASAN=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

Leak detection is turned off because the MSVC AddressSanitizer runtime on
Windows reports that `detect_leaks` is not supported on the platform. The
memory-error detection that does work is what caught the one lifetime defect
recorded in the release notes.

### Multiprocess proof

The multiprocess suite starts `podfabd` processes against a store directory,
talks to them over framed loopback TCP, terminates one without warning,
restarts it, and verifies that the new incarnation advanced the epoch, that a
lease minted by the killed incarnation is no longer honoured, and that recovered
expectations still fence an older generation.

---

## Telemetry

Pod Fabric transmits no telemetry and makes no outbound connection of its own.
The only network surface is a listener on an address the operator supplies, and
the clients that connect to it.

---

## Repository layout

```
include/podfabric/   public headers
src/                 implementation
apps/                podfabd, podfabricctl
examples/            examples and the independent find_package consumer
benchmarks/          completed-work benchmark
tests/               unit, property, adversarial, concurrency, multiprocess
cmake/               package configuration template
```

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
