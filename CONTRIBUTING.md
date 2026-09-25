# Contributing to Pod Fabric

Thanks for your interest. This document describes how contributions are
accepted.

## Licence

Pod Fabric is licensed under the Apache License, Version 2.0. By submitting a
contribution you agree that your contribution is licensed under the same terms,
as described in section 5 of that licence ("Submission of Contributions").
There is **no Contributor Licence Agreement to sign** and no copyright
assignment. You keep the copyright on your work.

Every new file must carry the SPDX header used throughout the tree:

```
// Copyright <year> <your name or organisation>
// SPDX-License-Identifier: Apache-2.0
```

## Ground rules

1. **Evidence over assertion.** A change that turns missing evidence into a
   success is rejected. UNKNOWN, UNSUPPORTED, STALE, CONFLICTING, INCOMPLETE,
   INDETERMINATE, REFUSED, CANCELLED and INVALID are distinct outcomes and must
   stay distinct.
2. **Determinism.** Composition must be a pure function of its request.
   Identical requests must produce byte-identical state documents. Add a
   property test when you touch composition.
3. **Bounds first.** Every external size is validated before it is used to
   allocate. Use the checked-arithmetic helpers rather than raw operators on
   externally supplied magnitudes.
4. **No fabricated capability.** Do not describe behaviour that has not been
   demonstrated. If a claim depends on hardware or a vendor stack that the
   project does not integrate with, say so explicitly and mark it UNSUPPORTED.
5. **Honest persistence.** Recovered dynamic evidence is historical. Never let
   a restart resurrect authority that a previous process incarnation held.
6. **No test timeouts.** A hang is a defect. Do not add a CTest TIMEOUT, a
   shell timeout wrapper, or logic that classifies a forced termination as a
   pass.

## Building and testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The build is warning-clean with -Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Werror on GCC/Clang and /W4 /WX on MSVC. Keep it that way; a
warning is a defect.

## Concurrency

Every public type documents its lock order. If you add a lock, write down where
it sits in the order and prove it with the concurrency tests. Callbacks are
never invoked while a lock is held; keep it that way.

## Pull requests

- One topic per change.
- Include the tests that demonstrate the behaviour.
- State plainly what you verified and what you did not. "Not verified" is an
  acceptable answer; an unverified claim is not.
