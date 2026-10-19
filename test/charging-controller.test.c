#include "charging-controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #expression);                                             \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

struct mock_authenticator {
    enum charging_auth_result result;
    unsigned int starts;
    unsigned int cancels;
    uint64_t started_request;
    uint64_t cancelled_request;
};

static enum charging_auth_result mock_start(void *context, uint64_t request_id)
{
    struct mock_authenticator *mock = context;

    mock->starts += 1;
    mock->started_request = request_id;
    return mock->result;
}

static void mock_cancel(void *context, uint64_t request_id)
{
    struct mock_authenticator *mock = context;

    mock->cancels += 1;
    mock->cancelled_request = request_id;
}

static void setup(struct charging_controller *controller,
                  struct mock_authenticator *mock)
{
    CHECK(charging_controller_init(
        controller,
        (struct charging_authenticator){mock_start, mock_cancel, mock},
        CHARGING_DEFAULT_TIMEOUT_MS));
}

static void connect_pd(struct charging_controller *controller)
{
    charging_controller_set_connection(
        controller, (struct charging_connection){true, CHARGING_TYPE_USB_PD,
                                                 false});
}

static void check_state(const struct charging_controller *controller,
                        enum charging_state expected)
{
    struct charging_snapshot snapshot = charging_controller_snapshot(controller);

    CHECK(snapshot.state == expected);
    CHECK(snapshot.pd_verified == (expected == CHARGING_STATE_VERIFIED));
    CHECK(snapshot.verify_process == (expected == CHARGING_STATE_VERIFYING));
    CHECK((snapshot.request_id != 0) == snapshot.verify_process);
}

static void eligible_types_require_success(void)
{
    const uint32_t types[] = {CHARGING_TYPE_USB_PD, CHARGING_TYPE_PD_PPS};

    for (size_t index = 0; index < sizeof(types) / sizeof(types[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = CHARGING_AUTH_VERIFIED};

        setup(&controller, &mock);
        check_state(&controller, CHARGING_STATE_DISCONNECTED);
        charging_controller_set_connection(
            &controller, (struct charging_connection){true, types[index], false});
        check_state(&controller, CHARGING_STATE_READY);
        CHECK(charging_controller_verify(&controller, 100) != 0);
        check_state(&controller, CHARGING_STATE_VERIFIED);
        CHECK(charging_controller_verify(&controller, 101) == 0);
        CHECK(mock.starts == 1);
        CHECK(mock.cancels == 0);
    }
}

static void ineligible_connections_do_not_authenticate(void)
{
    const struct charging_connection connections[] = {
        {false, CHARGING_TYPE_USB_PD, false},
        {true, CHARGING_TYPE_UNKNOWN, false},
        {true, 9, false},
        {true, UINT32_MAX, false},
        {true, CHARGING_TYPE_USB_PD, true},
        {true, CHARGING_TYPE_PD_PPS, true}
    };

    for (size_t index = 0;
         index < sizeof(connections) / sizeof(connections[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = CHARGING_AUTH_VERIFIED};

        setup(&controller, &mock);
        charging_controller_set_connection(&controller, connections[index]);
        CHECK(charging_controller_verify(&controller, 0) == 0);
        check_state(&controller, connections[index].online
                                     ? CHARGING_STATE_INELIGIBLE
                                     : CHARGING_STATE_DISCONNECTED);
        CHECK(mock.starts == 0);
    }
}

static void authentication_failures_are_terminal(void)
{
    const enum charging_auth_result results[] = {
        CHARGING_AUTH_REJECTED, CHARGING_AUTH_ERROR,
        (enum charging_auth_result)99
    };

    for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = results[index]};

        setup(&controller, &mock);
        connect_pd(&controller);
        CHECK(charging_controller_verify(&controller, 0) != 0);
        check_state(&controller, results[index] == CHARGING_AUTH_REJECTED
                                     ? CHARGING_STATE_REJECTED
                                     : CHARGING_STATE_ERROR);
        CHECK(charging_controller_verify(&controller, 1) == 0);
        CHECK(mock.starts == 1);
    }
}

static void pending_requests_are_deduplicated(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 100);
    CHECK(request_id != 0);
    CHECK(mock.started_request == request_id);
    check_state(&controller, CHARGING_STATE_VERIFYING);
    CHECK(charging_controller_verify(&controller, 101) == request_id);
    CHECK(mock.starts == 1);
    CHECK(charging_controller_complete(&controller, request_id,
                                        CHARGING_AUTH_VERIFIED, 102));
    check_state(&controller, CHARGING_STATE_VERIFIED);
}

static void disconnect_cancels_and_rejects_late_success(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 0);
    charging_controller_disconnect(&controller);
    CHECK(mock.cancels == 1);
    CHECK(mock.cancelled_request == request_id);
    CHECK(!charging_controller_complete(&controller, request_id,
                                         CHARGING_AUTH_VERIFIED, 1));
    check_state(&controller, CHARGING_STATE_DISCONNECTED);
    charging_controller_disconnect(&controller);
    CHECK(mock.cancels == 1);
}

static void timeout_cancels_at_the_deadline(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 100);
    charging_controller_tick(&controller, 5099);
    check_state(&controller, CHARGING_STATE_VERIFYING);
    charging_controller_tick(&controller, 5100);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
    CHECK(mock.cancels == 1);
    CHECK(mock.cancelled_request == request_id);
    CHECK(!charging_controller_complete(&controller, request_id,
                                         CHARGING_AUTH_VERIFIED, 5101));
    charging_controller_tick(&controller, 5200);
    CHECK(mock.cancels == 1);
}

static void initialization_validates_arguments(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    struct charging_authenticator authenticator = {mock_start, mock_cancel, &mock};
    struct charging_snapshot snapshot;

    CHECK(!charging_controller_init(NULL, authenticator, 1));
    CHECK(!charging_controller_init(&controller,
                                     (struct charging_authenticator){0}, 1));
    CHECK(!charging_controller_init(&controller, authenticator, 0));
    CHECK(!charging_controller_init(&controller, authenticator,
                                     (uint32_t)INT32_MAX + 1));
    CHECK(!charging_controller_init(&controller, authenticator, UINT32_MAX));
    CHECK(charging_controller_init(&controller, authenticator, INT32_MAX));
    check_state(&controller, CHARGING_STATE_DISCONNECTED);
    snapshot = charging_controller_snapshot(&controller);
    CHECK(!snapshot.connection.online);
    CHECK(snapshot.connection.charge_type == CHARGING_TYPE_UNKNOWN);
    CHECK(!snapshot.connection.peer_is_phone);

    CHECK(charging_controller_init(&controller, authenticator, 1));
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 0) != 0);
    charging_controller_tick(&controller, 0);
    check_state(&controller, CHARGING_STATE_VERIFYING);
    charging_controller_tick(&controller, 1);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
}

static void identical_connections_preserve_the_request_and_deadline(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 100);
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 5099) == request_id);
    CHECK(mock.starts == 1);
    CHECK(mock.cancels == 0);
    charging_controller_tick(&controller, 5100);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
}

static void reconnect_isolates_old_completions(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t old_request;
    uint64_t new_request;

    setup(&controller, &mock);
    connect_pd(&controller);
    old_request = charging_controller_verify(&controller, 100);
    charging_controller_disconnect(&controller);
    connect_pd(&controller);
    new_request = charging_controller_verify(&controller, 200);
    CHECK(new_request != 0 && new_request != old_request);
    CHECK(mock.starts == 2);
    CHECK(mock.cancels == 1);
    CHECK(!charging_controller_complete(&controller, old_request,
                                         CHARGING_AUTH_VERIFIED, 201));
    check_state(&controller, CHARGING_STATE_VERIFYING);
    CHECK(charging_controller_snapshot(&controller).request_id == new_request);
    CHECK(charging_controller_complete(&controller, new_request,
                                        CHARGING_AUTH_VERIFIED, 202));
    CHECK(!charging_controller_complete(&controller, old_request,
                                         CHARGING_AUTH_REJECTED, 203));
    check_state(&controller, CHARGING_STATE_VERIFIED);
    charging_controller_disconnect(&controller);
    check_state(&controller, CHARGING_STATE_DISCONNECTED);
    CHECK(mock.cancels == 1);
}

static void connection_changes_cancel_work_and_clear_trust(void)
{
    const struct charging_connection connections[] = {
        {true, CHARGING_TYPE_PD_PPS, false},
        {true, 9, false},
        {true, CHARGING_TYPE_USB_PD, true},
        {false, CHARGING_TYPE_USB_PD, false}
    };
    const enum charging_state states[] = {
        CHARGING_STATE_READY, CHARGING_STATE_INELIGIBLE,
        CHARGING_STATE_INELIGIBLE, CHARGING_STATE_DISCONNECTED
    };

    for (size_t index = 0;
         index < sizeof(connections) / sizeof(connections[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
        uint64_t request_id;

        setup(&controller, &mock);
        connect_pd(&controller);
        request_id = charging_controller_verify(&controller, 0);
        charging_controller_set_connection(&controller, connections[index]);
        CHECK(mock.cancels == 1);
        CHECK(mock.cancelled_request == request_id);
        CHECK(!charging_controller_complete(&controller, request_id,
                                             CHARGING_AUTH_VERIFIED, 1));
        check_state(&controller, states[index]);

        charging_controller_disconnect(&controller);
        connect_pd(&controller);
        mock.result = CHARGING_AUTH_VERIFIED;
        CHECK(charging_controller_verify(&controller, 2) != 0);
        check_state(&controller, CHARGING_STATE_VERIFIED);
        charging_controller_set_connection(&controller, connections[index]);
        check_state(&controller, states[index]);
        CHECK(mock.cancels == 1);
    }
}

static void completion_checks_the_deadline_without_a_tick(void)
{
    const uint64_t elapsed[] = {4999, 5000, 5001};

    for (size_t index = 0; index < sizeof(elapsed) / sizeof(elapsed[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
        bool on_time = elapsed[index] < CHARGING_DEFAULT_TIMEOUT_MS;
        uint64_t request_id;

        setup(&controller, &mock);
        connect_pd(&controller);
        request_id = charging_controller_verify(&controller, 100);
        CHECK(charging_controller_complete(&controller, request_id,
                                             CHARGING_AUTH_VERIFIED,
                                             100 + elapsed[index]) == on_time);
        check_state(&controller, on_time ? CHARGING_STATE_VERIFIED
                                         : CHARGING_STATE_TIMED_OUT);
        CHECK(mock.cancels == (on_time ? 0U : 1U));
    }
}

static void asynchronous_failures_and_invalid_results_fail_closed(void)
{
    const enum charging_auth_result results[] = {
        CHARGING_AUTH_REJECTED, CHARGING_AUTH_ERROR,
        CHARGING_AUTH_PENDING, (enum charging_auth_result)99
    };

    for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
        struct charging_controller controller;
        struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
        uint64_t request_id;

        setup(&controller, &mock);
        connect_pd(&controller);
        request_id = charging_controller_verify(&controller, 0);
        CHECK(charging_controller_complete(&controller, request_id,
                                             results[index], 1));
        check_state(&controller, results[index] == CHARGING_AUTH_REJECTED
                                     ? CHARGING_STATE_REJECTED
                                     : CHARGING_STATE_ERROR);
        CHECK(charging_controller_verify(&controller, 2) == 0);
        CHECK(mock.starts == 1);
    }
}

static void foreign_and_duplicate_completions_are_ignored(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 0);
    CHECK(!charging_controller_complete(&controller, 0,
                                         CHARGING_AUTH_VERIFIED, 1));
    CHECK(!charging_controller_complete(&controller, request_id + 1,
                                         CHARGING_AUTH_VERIFIED, 1));
    check_state(&controller, CHARGING_STATE_VERIFYING);
    CHECK(charging_controller_complete(&controller, request_id,
                                        CHARGING_AUTH_VERIFIED, 2));
    CHECK(!charging_controller_complete(&controller, request_id,
                                         CHARGING_AUTH_REJECTED, 3));
    check_state(&controller, CHARGING_STATE_VERIFIED);
}

static void cancellation_callback_is_optional(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    CHECK(charging_controller_init(
        &controller, (struct charging_authenticator){mock_start, NULL, &mock}, 10));
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 0);
    charging_controller_disconnect(&controller);
    CHECK(!charging_controller_complete(&controller, request_id,
                                         CHARGING_AUTH_VERIFIED, 1));
    check_state(&controller, CHARGING_STATE_DISCONNECTED);
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 2) != 0);
    charging_controller_tick(&controller, 12);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
    CHECK(mock.cancels == 0);
}

static void a_clock_before_the_start_fails_closed(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    uint64_t request_id;

    setup(&controller, &mock);
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, 100);
    charging_controller_tick(&controller, 99);
    check_state(&controller, CHARGING_STATE_ERROR);
    CHECK(mock.cancels == 1);
    CHECK(!charging_controller_complete(&controller, request_id,
                                         CHARGING_AUTH_VERIFIED, 101));
}

static void timeout_arithmetic_does_not_overflow(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};
    struct charging_authenticator authenticator = {mock_start, mock_cancel, &mock};
    uint64_t request_id;

    CHECK(charging_controller_init(&controller, authenticator, 10));
    connect_pd(&controller);
    request_id = charging_controller_verify(&controller, UINT64_MAX - 5);
    charging_controller_tick(&controller, UINT64_MAX);
    check_state(&controller, CHARGING_STATE_VERIFYING);
    CHECK(charging_controller_complete(&controller, request_id,
                                        CHARGING_AUTH_VERIFIED, UINT64_MAX));
    check_state(&controller, CHARGING_STATE_VERIFIED);

    CHECK(charging_controller_init(&controller, authenticator, 5));
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, UINT64_MAX - 5) != 0);
    charging_controller_tick(&controller, UINT64_MAX);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
}

static void request_ids_never_wrap(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};

    setup(&controller, &mock);
    controller.last_request_id = UINT64_MAX - 1;
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 0) == UINT64_MAX);
    CHECK(charging_controller_complete(&controller, UINT64_MAX,
                                        CHARGING_AUTH_VERIFIED, 1));
    charging_controller_disconnect(&controller);
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 2) == 0);
    check_state(&controller, CHARGING_STATE_ERROR);
    CHECK(mock.starts == 1);
}

static void repeated_verification_cannot_extend_a_timeout(void)
{
    struct charging_controller controller;
    struct mock_authenticator mock = {.result = CHARGING_AUTH_PENDING};

    setup(&controller, &mock);
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 0) != 0);
    CHECK(charging_controller_verify(&controller, 5000) == 0);
    check_state(&controller, CHARGING_STATE_TIMED_OUT);
    connect_pd(&controller);
    CHECK(charging_controller_verify(&controller, 5001) == 0);
    CHECK(mock.starts == 1);
    CHECK(mock.cancels == 1);
}

static void state_names_are_stable(void)
{
    const enum charging_state states[] = {
        CHARGING_STATE_DISCONNECTED, CHARGING_STATE_INELIGIBLE,
        CHARGING_STATE_READY, CHARGING_STATE_VERIFYING,
        CHARGING_STATE_VERIFIED, CHARGING_STATE_REJECTED,
        CHARGING_STATE_TIMED_OUT, CHARGING_STATE_ERROR
    };
    const char *const names[] = {
        "disconnected", "ineligible", "ready", "verifying",
        "verified", "rejected", "timed-out", "error"
    };

    for (size_t index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
        CHECK(strcmp(charging_state_name(states[index]), names[index]) == 0);
    }
    CHECK(strcmp(charging_state_name((enum charging_state)99), "unknown") == 0);
}

static unsigned int tests_run;

static void run_test(const char *name, void (*test_function)(void))
{
    test_function();
    tests_run += 1;
    printf("ok %u - %s\n", tests_run, name);
}

int main(void)
{
    run_test("eligible types require success", eligible_types_require_success);
    run_test("ineligible connections do not authenticate",
             ineligible_connections_do_not_authenticate);
    run_test("authentication failures are terminal",
             authentication_failures_are_terminal);
    run_test("pending requests are deduplicated", pending_requests_are_deduplicated);
    run_test("disconnect cancels and rejects late success",
             disconnect_cancels_and_rejects_late_success);
    run_test("timeout cancels at the deadline", timeout_cancels_at_the_deadline);
    run_test("initialization validates arguments", initialization_validates_arguments);
    run_test("identical connections preserve the request and deadline",
             identical_connections_preserve_the_request_and_deadline);
    run_test("reconnect isolates old completions", reconnect_isolates_old_completions);
    run_test("connection changes cancel work and clear trust",
             connection_changes_cancel_work_and_clear_trust);
    run_test("completion checks the deadline without a tick",
             completion_checks_the_deadline_without_a_tick);
    run_test("asynchronous failures and invalid results fail closed",
             asynchronous_failures_and_invalid_results_fail_closed);
    run_test("foreign and duplicate completions are ignored",
             foreign_and_duplicate_completions_are_ignored);
    run_test("cancellation callback is optional", cancellation_callback_is_optional);
    run_test("a clock before the start fails closed",
             a_clock_before_the_start_fails_closed);
    run_test("timeout arithmetic does not overflow", timeout_arithmetic_does_not_overflow);
    run_test("request IDs never wrap", request_ids_never_wrap);
    run_test("repeated verification cannot extend a timeout",
             repeated_verification_cannot_extend_a_timeout);
    run_test("state names are stable", state_names_are_stable);
    printf("%u tests passed\n", tests_run);
    return EXIT_SUCCESS;
}