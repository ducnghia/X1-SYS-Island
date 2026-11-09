# Code optimization review

Reviewed and implemented: 2026-11-09, on top of v0.8.5 (`80ff894`). This replaces the
v0.7.0 review; all of its findings were addressed in v0.8.0–v0.8.2.

## Measurements

`tests\perf-bench.bat` builds an `/O1` microbenchmark from the production source. It needs
no Administrator access. The table shows three interleaved runs of the original and the
optimized code on the same machine, under a busy desktop (CPU 20–50 %, iGPU ~80 %).
Absolute values vary between runs, so compare the two columns, not individual numbers.

| Path | Before (3 runs) | After (3 runs) |
| --- | --- | --- |
| Compact animation frame (border + CPU label) | 887–1676 µs | 253–301 µs |
| Expanded animation frame | 973–3402 µs | 303–516 µs |
| CPU-label damage cell only | 709–1383 µs | 210–299 µs |
| `Telemetry::sample` (non-elevated, ~1000 GPU instances) | 3358–9390 µs | 2149–3373 µs |

On AC power the animation runs at 10 fps, so the compact island used roughly 9 ms of UI-thread CPU
per second before and about 3 ms after. Battery savings have not been measured.

## Implemented changes

### Power

- **Display off and session lock.** Telemetry and animation now pause like suspension when the
  session display turns off (`GUID_SESSION_DISPLAY_STATUS`) or the session is locked
  (`WTSRegisterSessionNotification`). Previously the app sampled every second and animated at
  10 fps while nobody could see it. A dimmed display still counts as visible. The 15-second
  health watchdog respects the pause. If Windows ever drops the resume notification, mouse
  input, a hotkey or a second launch proves the island is visible and resumes it. Suspension
  is still cleared only by the power-resume event.
- **EcoQoS worker.** The telemetry worker requests `ThreadPowerThrottling`, so Windows may
  schedule it on efficient cores. Windows versions that do not support it ignore the request.
- **Coalescable timers.** The animation timer allows 10 ms of tolerance on AC and 250 ms on battery.
  The health timer allows 5 s. Hover and re-show timers stay exact because their 1 s / 5 s
  behaviour is user-visible and tested.
- **MSR affinity.** Both registers are package-scoped, so the sensor read now pins the worker to
  the processor it is already running on, instead of always the first permitted processor
  (often a P-core). It falls back to the old choice when the current processor is outside the
  permitted mask.

### Rendering

- **Halo text.** The v0.8.5 CPU-label halo drew the label 24 extra times per frame with
  `DrawTextW`, repeating ellipsis layout each time. The displayed string and baseline are now
  computed once per font, and the halo uses `ExtTextOutW`. A regression test proves the halo
  output is pixel-identical for short, ellipsized and machine-detected labels at all offsets.
- **Border damage.** The innermost glow layer's bottom arcs reached two pixels outside the
  border damage strips, at (10, h−8) and (w−11, h−8). Partial repaints left them stale. Two
  1-pixel strips were added; they do not intersect any text, logo or expanded cell.
- **Fewer window calls.** Dragging and resizing now use one `SetWindowPos` that also reasserts
  topmost order, instead of two.

### Telemetry

- **Case-insensitive matching.** GPU instance names are ASCII, and LUID prefixes are generated
  in lowercase. Matching now uses ASCII folding instead of a `towlower` CRT call per character
  across about 1000 instances.
- **GPU buffer reuse.** The formatted-counter array is read directly into the retained buffer.
  The separate size query runs only when the buffer is too small.
- **No UI string copies.** The UI copies only the numeric `Readings` slice of `Stats`, so the
  diagnostic strings are never copied under the mutex. Display strings are swapped instead of
  copied to detect changes.

### Maintenance

- Colour logic moved to `island_style.h` and registry settings to `settings.h`. Settings are
  written with `RegSetKeyValueW` through one helper.
- Removed dead code: the never-armed `STATS_TIMER_ID` and the unreachable `WM_COMMAND` About
  branch. The context menu uses a `MenuCommand` enum.
- `positionRight` again stacks below a running AI Island when they would overlap. The previous
  statement was a no-op (`max(y, rcWork.top)`), inconsistent with the reserved-footprint check
  immediately below it.
- `tools\vsenv.bat` locates any Visual Studio 2022 edition through `vswhere`. Every build and
  test script uses it. `tests\run-all.bat` runs every non-interactive test.
- The five per-version install scripts became `tests\install-release.ps1`. Its expected version
  defaults to the one in `x1_sys_island.rc`.
- `tests\hover-test.cpp` no longer assumes the CPU label is `CPU`. It failed on any machine with
  a detected CPU identity, and its call-count assertions had not been updated for the v0.8.5
  halo.

## Deliberately not changed

- **GPU sampling cadence.** The `GPU Engine(*)` counter is still the most expensive telemetry
  work, at about 1.3–3 ms per sample. A partial-wildcard instance filter (`*luid_…*`) did not
  reduce the cost in a measurement, because the provider still enumerates every engine.
  Sampling the iGPU less often than CPU/RAM would help, but it changes how fresh the displayed
  value is, which is a product decision.
- **Hover state machine.** The six hover/visibility flags would read better as one state enum.
  The deterministic hover tests drive those globals directly, though, so the refactor would mean
  rewriting the tests without a behavioural benefit.
- **Per-monitor DPI scaling.** The process declares Per-Monitor V2 awareness, but all geometry,
  fonts and the logo bitmap are fixed pixel sizes, and `WM_DPICHANGED` is not handled. Scaling
  changes the island's appearance on every non-100 % display and needs visual sign-off. It is
  a feature, not an optimization.
- **Persistent fan mapping, cached TjMax, compiler flags.** Same reasoning as the earlier review:
  correctness first, and no measured hotspot.
- **CI.** The hover tests need an interactive desktop and timer-accurate message pumping. They
  have not been validated on a hosted runner.

## Validation

- `tests\run-all.bat`: sensor-optimization, hover/render (13 groups, including the new halo
  equivalence and display/lock pause tests) and live telemetry diagnostics all pass.
- `build.bat build\X1-SYS-Island-review.exe`: clean optimized `/W4` build.
- Not run here: `tests\test.bat --require-temperature`, which needs an elevated prompt with
  PawnIO, and a long-duration suspend/lock/display-off soak on hardware.
