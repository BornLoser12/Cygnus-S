# Stability update

## Scope

- Both halves declare the existing key matrix as a deep-sleep wakeup source.
- Gesture movement and key press/release state are owned by the system workqueue.
  The input thread only queues bounded, non-blocking messages. Layer 8 transitions
  invalidate old queued samples and clear partial movement, cooldown and held keys.
  Overflow invalidates partial frames rather than producing stale gestures.
- BLE configuration, keymap, runtime combo defaults, AML defaults, trackball driver,
  sensor timing, CPI, axis directions, precision scaling and gesture sensitivity
  are unchanged. Stored DYA settings remain in effect. No reset-on-start is added
  to either normal firmware.
- No automatic PMW3610 reset/recovery is added without failure logs.

## Reproducible baseline

Dependency commits, SDK container digest and CI action commits are taken from the
successful build of `48f95d8`, Actions run
<https://github.com/BornLoser12/Cygnus-S/actions/runs/35343870812>.
The 26 direct dependency SHAs were checked against that run's update log. Imported
manifests are immutable because their parent commits are pinned; CI also rejects
any resolved dependency that still refers to a branch or tag.

Every build archives `build-diagnostics`: the resolved/frozen dependency manifests,
full build log, each target's final `.config`, device tree, ELF and link map. The
ordinary `firmware` artifact still contains the existing UF2 filenames.

## Validation

`python3 tests/run_gesture_tests.py` compiles the actual C gesture function bodies
against deterministic host doubles. Tests cover direction, 133-count sensitivity,
35 ms release / 120 ms cooldown, leaving/re-entering the layer, stale X/Y frames,
rapid direction changes, queue overflow, and error cleanup. They also assert
that key behaviors run only from the simulated system workqueue. This is not a
hardware concurrency or BLE test; the firmware build checks the actual Zephyr
API and DT registration separately.

CI verifies generated `.config`/DTS for both normal halves, including key wakeup,
unchanged BLE timing, and absence of settings reset. After flashing, test:

1. Flash normal `Cygnus_L.uf2` and `Cygnus_R.uf2`; do not use `SETTINGS_RESET` files.
2. With right on USB, leave left idle beyond its configured deep-sleep timeout,
   then press a left key and allow the inter-half connection to resume.
3. If used on battery/BLE, separately test right-side key wakeup too.
4. Enter Gesture, move less than the threshold, leave and re-enter, and confirm
   that a fresh full motion is needed. Check all four Win+arrow directions and
   rapid layer changes without a held Win/arrow key.
5. Confirm the existing DYA connection, keymap, combos, AML, precision and mouse
   direction. Firmware defaults do not overwrite the user's stored DYA values.

Do not diagnose a PMW3610-only failure as a BLE failure or assume the watchdog
will repair it: its workqueue watchdog is not a sensor-health monitor.
