#include "charging-controller.h"

#include <stddef.h>

static void cancel_authentication(struct charging_controller *controller,
                                  enum charging_state state)
{
    uint64_t request_id = controller->active_request_id;

    controller->active_request_id = 0;
    controller->state = state;
    if (request_id != 0 && controller->authenticator.cancel != NULL) {
        controller->authenticator.cancel(controller->authenticator.context,
                                         request_id);
    }
}

bool charging_controller_init(struct charging_controller *controller,
                              struct charging_authenticator authenticator,
                              uint32_t timeout_ms)
{
    if (controller == NULL || authenticator.start == NULL || timeout_ms == 0 ||
        timeout_ms > INT32_MAX) {
        return false;
    }

    *controller = (struct charging_controller){
        .authenticator = authenticator,
        .state = CHARGING_STATE_DISCONNECTED,
        .timeout_ms = timeout_ms
    };
    return true;
}

struct charging_snapshot charging_controller_snapshot(
    const struct charging_controller *controller)
{
    return (struct charging_snapshot){
        .connection = controller->connection,
        .state = controller->state,
        .verify_process = controller->state == CHARGING_STATE_VERIFYING,
        .pd_verified = controller->state == CHARGING_STATE_VERIFIED,
        .request_id = controller->active_request_id
    };
}

void charging_controller_set_connection(struct charging_controller *controller,
                                        struct charging_connection connection)
{
    enum charging_state state;

    if (connection.online == controller->connection.online &&
        connection.charge_type == controller->connection.charge_type &&
        connection.peer_is_phone == controller->connection.peer_is_phone) {
        return;
    }

    if (!connection.online) {
        state = CHARGING_STATE_DISCONNECTED;
    } else if (connection.peer_is_phone ||
               (connection.charge_type != CHARGING_TYPE_USB_PD &&
                connection.charge_type != CHARGING_TYPE_PD_PPS)) {
        state = CHARGING_STATE_INELIGIBLE;
    } else {
        state = CHARGING_STATE_READY;
    }

    controller->connection = connection;
    cancel_authentication(controller, state);
}

void charging_controller_disconnect(struct charging_controller *controller)
{
    charging_controller_set_connection(controller,
                                       (struct charging_connection){0});
}

void charging_controller_tick(struct charging_controller *controller,
                              uint64_t now_ms)
{
    if (controller->state != CHARGING_STATE_VERIFYING) {
        return;
    }
    if (now_ms < controller->started_ms) {
        cancel_authentication(controller, CHARGING_STATE_ERROR);
    } else if (now_ms - controller->started_ms >= controller->timeout_ms) {
        cancel_authentication(controller, CHARGING_STATE_TIMED_OUT);
    }
}

bool charging_controller_complete(struct charging_controller *controller,
                                  uint64_t request_id,
                                  enum charging_auth_result result,
                                  uint64_t now_ms)
{
    if (controller->state != CHARGING_STATE_VERIFYING || request_id == 0 ||
        request_id != controller->active_request_id) {
        return false;
    }

    charging_controller_tick(controller, now_ms);
    if (controller->state != CHARGING_STATE_VERIFYING ||
        request_id != controller->active_request_id) {
        return false;
    }

    switch (result) {
    case CHARGING_AUTH_VERIFIED:
        controller->state = CHARGING_STATE_VERIFIED;
        break;
    case CHARGING_AUTH_REJECTED:
        controller->state = CHARGING_STATE_REJECTED;
        break;
    case CHARGING_AUTH_ERROR:
        controller->state = CHARGING_STATE_ERROR;
        break;
    default:
        cancel_authentication(controller, CHARGING_STATE_ERROR);
        return true;
    }
    controller->active_request_id = 0;
    return true;
}

uint64_t charging_controller_verify(struct charging_controller *controller,
                                    uint64_t now_ms)
{
    uint64_t request_id;
    enum charging_auth_result result;

    charging_controller_tick(controller, now_ms);
    if (controller->state == CHARGING_STATE_VERIFYING) {
        return controller->active_request_id;
    }
    if (controller->state != CHARGING_STATE_READY) {
        return 0;
    }
    if (controller->last_request_id == UINT64_MAX) {
        controller->state = CHARGING_STATE_ERROR;
        return 0;
    }

    request_id = ++controller->last_request_id;
    controller->active_request_id = request_id;
    controller->started_ms = now_ms;
    controller->state = CHARGING_STATE_VERIFYING;
    result = controller->authenticator.start(controller->authenticator.context,
                                              request_id);
    if (result != CHARGING_AUTH_PENDING) {
        charging_controller_complete(controller, request_id, result, now_ms);
    }
    return request_id;
}

const char *charging_state_name(enum charging_state state)
{
    switch (state) {
    case CHARGING_STATE_DISCONNECTED:
        return "disconnected";
    case CHARGING_STATE_INELIGIBLE:
        return "ineligible";
    case CHARGING_STATE_READY:
        return "ready";
    case CHARGING_STATE_VERIFYING:
        return "verifying";
    case CHARGING_STATE_VERIFIED:
        return "verified";
    case CHARGING_STATE_REJECTED:
        return "rejected";
    case CHARGING_STATE_TIMED_OUT:
        return "timed-out";
    case CHARGING_STATE_ERROR:
        return "error";
    }
    return "unknown";
}