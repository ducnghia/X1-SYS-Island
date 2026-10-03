# Code optimization review and plan

Reviewed: 2026-10-03. Scope: unpacked v0.7.0 source, not the older ZIP archives.

This is a static review. No application code was changed, and no builds, tests, or performance measurements were run. Priorities below reflect likely value and risk, not measured speedups.

## v0.8.0 implementation status

Implemented layout caching, stock-pen and logo-DC reuse, bounded hover-hide telemetry retention with resume priming, unchanged-policy wakeup suppression, allocation-free matching of unrelated GPU names, fresh sensor failure retry deadlines, and improved sensor/affinity diagnostics. Added deterministic sensor and worker/layout lifecycle regression tests.

The optimized release build and all three test scripts passed. Elevated live diagnostics returned CPU Package/TjMax and fan readings. Installed executable was backed up, replaced, hash-verified, and launched successfully at `C:\Users\Admin\AppData\Local\Programs\X1 SYS Island\X1-SYS-Island.exe`; installation transcript: `build/v080-install.log`. The elevation wait command timed out, but subsequent transcript and process inspection confirmed successful installation and a running replacement process.

Partial repainting, persistent fan mapping, cached TjMax, and compiler flag changes remain deliberately deferred as described below. No before/after performance baseline or long-duration battery/suspend test was performed; speedup and battery savings remain unmeasured.

## Summary

The existing architecture is appropriate for a small Win32 overlay: sensors run on a below-normal-priority worker, formatted text is cached outside painting, fonts and the background brush are reused, sampling stops while hidden or suspended, and battery operation reduces sampling and disables animated repainting. Keep these properties rather than introducing a framework or rewriting the renderer.

The best initial optimization candidate is repeated work in the 100 ms animation paint path. Next, examine initialization churn during temporary hover hides and GPU counter processing. Fix sensor recovery/error handling before attempting hardware-data caching.

## Findings, in priority order

### 1. Medium: unchanged content is remeasured and repainted on every AC animation tick

Evidence: `x1_sys_island.cpp:68`, `370–449`, `508–519`.

On AC, the timer invalidates the entire window nominally ten times per second. Each paint creates/deletes a pen and a logo memory DC, measures all four compact strings plus the CPU label, draws the logo and compact values, and redraws six expanded fields when expanded. Telemetry ordinarily changes only once per second. Both the border and CPU label animate, so an optimization must preserve both.

Plan:

- Cache text extents and compact positions when displayed strings, font, or window dimensions change. Cache the fixed CPU-label extent separately.
- Replace the temporary solid pen with the stock `DC_PEN` and `SetDCPenColor`, restoring selected objects afterward.
- Consider retaining the logo source DC, with explicit creation, selection restoration, and destruction.
- Measure before introducing backing bitmaps or partial invalidation. If justified, invalidate only the border and animated CPU-label regions between telemetry updates; the paint routine must also avoid unnecessary layout work. Invalidate the full view on resize, data changes, and exposure as needed.

Expected benefit: fewer GDI calls and less repeated text work. Actual CPU/power improvement is unknown because the window is small.

Validation: compare compact/expanded screenshots, pulse timing, label colors, clipping, dragging, and repeated resize cycles. Track paint duration and GDI handle counts over a long run.

### 2. Medium: temporary hover hiding rebuilds all telemetry resources

Evidence: `x1_sys_island.cpp:461–464`, `522–530`, `715–740`; `telemetry.h:51–75`; `cpu_temperature.h:44–78`.

Every hide disables sampling. The worker destroys `Telemetry`, closing PDH and PawnIO. Showing again constructs a DXGI factory, enumerates adapters, opens/adds/primes counters, and later reloads the sensor module. This is reasonable for a long manual hide, but the same teardown happens for the normal five-second hover hide. The first new sample also waits for the sampling interval after counter priming.

Plan:

- Measure reconstruction latency and frequency during repeated hover cycles before changing resource lifetime.
- If material, distinguish temporary hover hiding from manual hiding and suspension. Consider a short retention grace period only for hover hiding, while performing no sampling during the hidden interval.
- Preserve immediate resource release for suspension and long/manual hiding unless requirements change.
- Explicitly re-prime rate counters on resume rather than reporting an average covering the hidden interval as current load.

Tradeoff: retaining the PawnIO device/module extends privileged resource lifetime. Keep the current policy if reconstruction costs are negligible.

Validation: repeated hover hide/show, manual hiding during a hover timeout, power suspend/resume, prompt exit while hidden, resource counts, and time to a fresh sample.

### 3. Medium: GPU aggregation allocates and normalizes names for unrelated counters

Evidence: `telemetry.h:92–111`.

The wildcard PDH array contains process/engine instances across adapters. Each valid name is copied and lowercased before matching an Intel LUID. Matching entries allocate suffix keys in a fresh `std::map`.

Plan:

- Measure PDH collection separately from name parsing and aggregation, with realistic and large instance counts.
- If parsing matters, perform case-insensitive matching without allocating a full string for every instance; materialize keys only for matches.
- Benchmark a reusable aggregation container rather than assuming `unordered_map` is faster for a small number of engines.
- Keep views into the PDH buffer local to one sample. The existing byte vector already retains capacity; its `resize()` is not necessarily allocating each time.

Correctness constraint: sum processes sharing the same adapter/physical-engine identity, then choose the busiest engine. Do not group solely by engine type or sum unrelated engines.

Validation: multiple processes on one engine, multiple adapters and physical engines, mixed-case names, invalid counters, and exact output equivalence to the current implementation.

### 4. Medium: sensor read failures do not establish a new retry deadline

Evidence: `cpu_temperature.h:44–47`, `118–120`.

The ten-second retry deadline is set when opening the device. When an established connection fails after that deadline has passed, `close()` leaves an expired deadline, allowing another open/module load on the next sample. Subsequent attempts are still throttled by `open()`; this is not an unbounded tight retry loop.

Plan: set a fresh deadline on transport/read failure. Consider bounded exponential backoff only if recurring failures warrant it, and reset it after successful readings. Keep recovery responsive after a transient failure.

Validation: inject a read failure after more than ten seconds of successful operation, then count device-open and module-load attempts against a controllable clock. Check sustained failure and successful recovery.

### 5. Medium/low: improve sensor failure diagnostics before optimizing driver calls

Evidence: `cpu_temperature.h:39–42`, `105–120`.

- Affinity restoration is unchecked. If it fails, the worker can remain pinned to one logical processor; actual occurrence has not been reproduced.
- A successful IOCTL with an incorrect byte count reports failure with `lastError == ERROR_SUCCESS`.
- Invalid decoded thermal data can likewise produce an unavailable message containing error zero.

Plan: check affinity restoration and distinguish transport errors, malformed replies, and invalid sensor readings. Use a scoped affinity guard if refactoring the sampling function, with a way to surface restoration failure.

Validation: injected affinity failures, short IOCTL replies, known transport errors, invalid thermal values, and successful restoration of the original group/mask.

### 6. Low: watchdog wakeups are unconditional

Evidence: `x1_sys_island.cpp:142–147`, `490–495`, `520–521`, `719–740`.

Every fifteen seconds the health timer signals the worker, including while hidden. On active policy events the worker resets its timeout without sampling. This creates some avoidable wakeups and can shift sample timing, but the low frequency suggests a small potential gain.

Plan: retain the documented logon-race recovery. Only optimize after measuring: signal on genuine policy transitions and use an explicit worker-health condition for recovery, or preserve a monotonic next-sample deadline across unchanged policy events.

Validation: early-logon visibility races, stable cadence, hidden/suspended wakeup counts, and recovery after missing initial visibility signals. Do not remove the watchdog without a replacement regression test.

## Optimizations to defer

- **Persistent fan mapping:** `fan_reader.h:29–49` intentionally reopens the read-only mapping each sample to handle service restarts. Caching can retain an obsolete mapping and affect its lifetime. Keep this unless measured cost justifies a tested stale-data/reopen policy. Preserve sequence checks, barriers, and the five-second freshness limit.
- **Cached TjMax:** `cpu_temperature.h:105–116` reads both registers every sample. Caching could remove one IOCTL, but requires evidence that the target remains valid on supported hardware and invalidation on reconnect/resume. Never trade temperature correctness for an unmeasured gain.
- **Compiler flag changes:** `build.bat:18` already uses `/O1`, `/GL`, `/LTCG`, dead-code elimination, and identical COMDAT folding. Benchmark `/O2` only after runtime hotspots are known; larger code is not automatically faster for this workload.
- **Framework or lock-free rewrites:** the short snapshot mutex and modest Win32 UI do not justify these without contention evidence.

## Implementation sequence

1. **Establish a baseline.** Measure process CPU time, working set, GDI/USER handles, paint duration, sensor/PDH latency, initialization latency, and sample cadence. Use lightweight timing or ETW/WPR without introducing synchronous per-frame logging.
2. **Fix recovery observability.** Add test seams for clock/device/affinity calls, then implement findings 4 and 5 with deterministic tests.
3. **Reduce paint overhead.** Implement layout caching and stock-pen reuse first. Add retained DCs or partial repainting only if their measured benefit warrants the lifecycle complexity.
4. **Optimize telemetry selectively.** Use the baseline to choose GPU parsing or temporary-hide resource retention; do not bundle both into an unmeasurable rewrite.
5. **Re-run the behavior and performance matrix.** Document before/after measurements and retain only changes with a useful benefit and no behavioral regression.

## Validation matrix

Run the existing scripts from the source directory on Windows with Visual Studio 2022 C++ Build Tools:

- `tests\test.bat`: telemetry helpers and live sampling checks.
- `tests\test-hover.bat`: deterministic UI/layout/hover behavior checks.
- `tests\test.bat --require-temperature`: elevated hardware validation on a supported machine with PawnIO available.
- `build.bat build\X1-SYS-Island-review.exe`: optimized build without replacing the root application executable.

Add focused tests for GPU parsing, retry timing, malformed IOCTL replies, affinity restoration, and temporary-hide lifecycle before changing those paths. Existing smoke tests alone do not prove optimization equivalence.

Compare equal-duration runs on the same machine and workload for AC compact, AC expanded, battery, manually hidden, repeated hover hides, suspend/resume, absent sensor/service, and many GPU-engine instances. Confirm stable handles/memory, unchanged displayed values, no sampling while inactive, and no added background topmost polling. Do not claim battery savings or percentage speedups without measurements.
