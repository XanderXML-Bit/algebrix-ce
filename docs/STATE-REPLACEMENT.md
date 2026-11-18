# Preserve state after a returned save failure

`ce_state_save` keeps its Boolean interface, IPC6, and the existing state layout.
All state writers use the same predecessor journal.

## Select the authoritative state

`CEOLD` holds the preceding state while a replacement is uncommitted.
The loader selects `CEOLD` by existence, not successful reading or header validation.
An unreadable or invalid predecessor causes defaults and a failed load.
It never selects a speculative replacement instead.

The OS AppVar lookup requires no handle, allocation, or retained data pointer.
The subsequent reader checks the full structure size, read count, magic, and version.

## Save a replacement

1. Rename `CE_STATE` to `CEOLD`.
2. Write the complete new `CE_STATE` and close its handle.
3. Delete `CEOLD` to commit.

The pinned CEdev v11.2 implementation checks the destination before the source.
Its rename results are zero for success, one for an existing destination, and two for an absent source.
Result two therefore means both names are absent.
The public header groups other errors differently, so this distinction depends on the inspected v11.2 source.
Unexpected rename results fail closed.

An existing destination means an earlier failed save already preserved the predecessor.
Retries overwrite only the speculative primary and retain that predecessor until commit.
A failed save invalidates `CE_RESP` through the common persistence path, including non-evaluating workers and forwarders.

## Bound the guarantee

The handled failures are returned create/open failures, zero or short data writes, nondestructive rename failures, and failed predecessor deletion.
The tests sweep every short prefix and repeated failures, then read through the actual `ce_state_load` implementation.
They also cover absent initial state, ordinary saves, retries, allocation exhaustion, and unreadable predecessors.

A complete write succeeds only when the library reports the expected count.
CEdev writes synchronously into an AppVar, and closing releases a handle without deferred flushing.
Existing-response deletion has no normal failure return except absence in the inspected library.
The synthetic cleanup failure targets the new predecessor deletion, not arbitrary OS exceptions.

This design does not guarantee reset or power-loss atomicity, concurrent-writer isolation, or recovery from silent corruption.
Nonreturning TI-OS allocation, archive, or garbage-collection exceptions remain outside the returned-error model.
The library rename copies, archives, and deletes variables rather than changing only their names.
It can require temporary RAM and archive space and can cause archive writes.
Physical hardware remains unverified.

## Keep module size within the ceiling

The journal initially grows the complete integrated CECISM to 51,331 B.
The repair shares read/write transfer plumbing and the existing bounded pending-name copy.
Forwarding copies the already terminated caller field into the equally sized return field.
No mathematical algorithm, evaluator cap, recursion guard, or step rule changes.
Every module must be rebuilt and measured before integration.
