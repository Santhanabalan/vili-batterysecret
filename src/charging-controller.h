#ifndef BATTERYSECRET_CHARGING_CONTROLLER_H
#define BATTERYSECRET_CHARGING_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#define CHARGING_DEFAULT_TIMEOUT_MS UINT32_C(5000)

enum charging_type {
    CHARGING_TYPE_UNKNOWN = 0,
    CHARGING_TYPE_USB_PD = 10,
    CHARGING_TYPE_PD_PPS = 11
};

enum charging_state {
    CHARGING_STATE_DISCONNECTED,
    CHARGING_STATE_INELIGIBLE,
    CHARGING_STATE_READY,
    CHARGING_STATE_VERIFYING,
    CHARGING_STATE_VERIFIED,
    CHARGING_STATE_REJECTED,
    CHARGING_STATE_TIMED_OUT,
    CHARGING_STATE_ERROR
};

enum charging_auth_result {
    CHARGING_AUTH_PENDING,
    CHARGING_AUTH_VERIFIED,
    CHARGING_AUTH_REJECTED,
    CHARGING_AUTH_ERROR
};

struct charging_connection {
    bool online;
    uint32_t charge_type;
    bool peer_is_phone;
};

struct charging_authenticator {
    enum charging_auth_result (*start)(void *context, uint64_t request_id);
    void (*cancel)(void *context, uint64_t request_id);
    void *context;
};

struct charging_snapshot {
    struct charging_connection connection;
    enum charging_state state;
    bool verify_process;
    bool pd_verified;
    uint64_t request_id;
};

struct charging_controller {
    struct charging_authenticator authenticator;
    struct charging_connection connection;
    enum charging_state state;
    uint32_t timeout_ms;
    uint64_t started_ms;
    uint64_t last_request_id;
    uint64_t active_request_id;
};

bool charging_controller_init(struct charging_controller *controller,
                              struct charging_authenticator authenticator,
                              uint32_t timeout_ms);
struct charging_snapshot charging_controller_snapshot(
    const struct charging_controller *controller);
void charging_controller_set_connection(struct charging_controller *controller,
                                        struct charging_connection connection);
void charging_controller_disconnect(struct charging_controller *controller);
uint64_t charging_controller_verify(struct charging_controller *controller,
                                    uint64_t now_ms);
bool charging_controller_complete(struct charging_controller *controller,
                                  uint64_t request_id,
                                  enum charging_auth_result result,
                                  uint64_t now_ms);
void charging_controller_tick(struct charging_controller *controller,
                              uint64_t now_ms);
const char *charging_state_name(enum charging_state state);

#endif