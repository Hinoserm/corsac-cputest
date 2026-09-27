# Deferred: Pentium Pro erratum 69

Status: SMP-only test proposal. Deferred at the user's request on
2026-09-27. No test implementation or execution has been added for it.

## Authority and required conditions

Intel Pentium Pro Processor Specification Update, 242689-035, erratum 69,
"EFLAGS Discrepancy on a Page Fault After a Multiprocessor TLB Shootdown,"
printed page 44 and the affected-stepping table on page 12.

The documented failure requires all of these conditions:

1. A read-modify-write instruction has a store pending while speculative
   loads evict its destination translation from the data TLB. Intel
   specifies at least three nearby loads with equal linear-address bits
   15:12 and different bits 31:16.
2. The destination page's permissions are tightened between that eviction
   and the store, causing a page fault.
3. Another processor makes the permission change without a synchronized
   TLB flush on the processor executing the instruction.

The saved arithmetic flags can describe the instruction's would-be
completed result even though its store faulted. This is not permission
to accept arbitrary flags in ordinary single-processor fault tests.

## Proposed bounded test

- Use a Pentium Pro BSP and a second running processor, a private page
  table and a dedicated RAM destination.
- Warm the writable destination translation, then race the BSP's affected
  instruction and conflicting speculative-load sequence against an AP
  changing the destination PTE from writable to read-only.
- Sweep bounded timing delays. Synchronize setup and cleanup, but do not
  flush or synchronize away the permission-change race itself.
- Use inputs that distinguish old arithmetic flags from completed-result
  flags. Check fault IP, page-fault error code, memory and register state;
  classify normal fault flags separately from the documented alternative.
- Record attempts, completed stores, normal page faults and observed
  premature flags. A run that does not hit the internal timing window
  must not be reported as reproducing or disproving the erratum.
- Restore the page mapping only after the AP has stopped modifying it.

## Limits and resumption

The existing fixed-permission single-processor cases do not create this
race. A main-only build without SMP cannot exercise the required second
processor; it must not substitute for this test. Executing the address
pattern also does not prove an emulator models speculative TLB eviction.

Leave this work deferred. Resume only on further user instruction, with
the requested manual source/book audit completed before execution.
