# Batterysecret-Inspired C Controller

A portable C11 implementation of the charging-authentication gate modeled in
this project. It builds as a static library and runs native tests with a mocked
authenticator. It does not execute the supplied Android binary or access charging
hardware.

## Build and Test

Requires a C11 compiler, Make, and `ar`. The controller and its tests have no
third-party library dependencies.

```sh
make
make test
make sanitize
```

- `make` builds `build/libcharging-controller.a`.
- `make test` builds and runs `build/charging-controller-test` with strict compiler
  warnings treated as errors.
- `make sanitize` runs the same tests in a separate build directory with AddressSanitizer
  and UndefinedBehaviorSanitizer. This requires a compiler supporting those sanitizers.
- `make clean` removes the default build's object, library, and test executable.
  Use `make BUILD_DIR=build/sanitized clean` for the sanitizer build.

## Integrate

The public API is in [src/charging-controller.h](src/charging-controller.h).
Compile [src/charging-controller.c](src/charging-controller.c) into your project,
or link the static library. The native
[test/charging-controller.test.c](test/charging-controller.test.c) demonstrates
the callback interface and event sequence with deterministic timestamps.

1. Initialize a `struct charging_controller` using `charging_controller_init`.
   Supply a required `start(context, request_id)` authentication callback, an
   optional `cancel(context, request_id)` callback, and a timeout from 1 through
   `INT32_MAX` milliseconds. `CHARGING_DEFAULT_TIMEOUT_MS` is 5000.
2. Publish connection changes using `charging_controller_set_connection`.
   Online types `CHARGING_TYPE_USB_PD` (10) and `CHARGING_TYPE_PD_PPS` (11) are
   eligible unless `peer_is_phone` is true. Other connections cannot authenticate.
3. Call `charging_controller_verify(controller, now_ms)` to begin authentication.
   Its nonzero return value identifies that attempt. Calls during verification
   return the same ID without starting another attempt or extending the deadline;
   zero means no request was started or remains active.
4. The `start` callback returns `CHARGING_AUTH_PENDING` for asynchronous work, or
   a terminal `CHARGING_AUTH_VERIFIED`, `CHARGING_AUTH_REJECTED`, or
   `CHARGING_AUTH_ERROR` result. It must only report verified after your actual
   authentication protocol has succeeded.
5. Deliver asynchronous terminal results through `charging_controller_complete`,
   with the original request ID and current time. A true return means the result
   was consumed, not that authentication succeeded. Unknown results, including
   using pending as a completion, fail closed. Expired, foreign, and stale
   completions return false.
6. Call `charging_controller_tick` from your event loop or timer while verifying.
   Supply time in milliseconds from one monotonic clock to every time-taking API.
   The controller does not create a timer or thread. Timeouts become visible on
   the next tick, verify, or matching completion; completion also checks the
   deadline so a late success cannot grant verification.
7. Read `charging_controller_snapshot`. `pd_verified` is true only in the
   verified state; `verify_process` is true only while verifying. The snapshot is
   a value copy, and `charging_state_name` provides a printable state label.
8. Call `charging_controller_disconnect` on removal. Connection changes and
   timeout clear the active ID before notifying `cancel`, so late results cannot
   affect a later attempt. A terminal result is retained until connection data
   changes; there is no automatic retry. Identical connection updates are no-ops.

Use these APIs on one thread, or serialize access externally. Callbacks must be
nonblocking and must not re-enter the controller. A blocking callback prevents
the caller from processing timeout ticks. Treat the controller's fields as
private; they are exposed only to allow allocation without a heap.

All calls other than initialization require a non-null, successfully initialized
controller. Keep it and the callback context alive until backend work has stopped
and all callbacks have drained. Do not reinitialize it while old completions can
still arrive. A physical adapter replacement must publish a disconnect before
reconnecting, even if its connection properties match the previous adapter.

## Scope and Evidence

This is independently written controller code, not recovered original source or
a complete replacement for `batterysecret`. The gate accepts a backend's
authentication result; it does not implement vendor PD messages, role swaps,
challenge-response cryptography, fuel-gauge authentication, sysfs writes, or
voltage/current/temperature policy. Those require a target-specific backend and
separate device testing. No backend that grants hardware charging permissions is
included.

The timeout, cancellation handling, and stale-result protection are implementation
choices, not claims of exact binary equivalence. The tests validate the C
controller with mocks, not the original daemon or a physical charger.
