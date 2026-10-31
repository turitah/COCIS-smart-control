#include "access_logic.h"

#include <string.h>

static int find_user(const access_state_t *state, const char *uid)
{
    size_t i;
    for (i = 0; i < state->count; ++i) {
        if (strcmp(state->users[i].uid, uid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void access_state_init(access_state_t *state, size_t capacity)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->capacity = capacity;
}

bool access_register_user(access_state_t *state, const char *uid)
{
    size_t len;
    if (state == NULL || uid == NULL || uid[0] == '\0') return false;
    len = strlen(uid);
    if (len > ACCESS_UID_MAX_LEN || state->count >= ACCESS_MAX_USERS) return false;
    if (find_user(state, uid) >= 0) return true; /* idempotent registration */
    (void)strncpy(state->users[state->count].uid, uid, ACCESS_UID_MAX_LEN);
    state->users[state->count].uid[ACCESS_UID_MAX_LEN] = '\0';
    state->users[state->count].authorized = true;
    state->users[state->count].inside = false;
    state->count++;
    return true;
}

access_result_t access_process_card(access_state_t *state,
                                    const char *uid,
                                    access_direction_t direction,
                                    bool schedule_open)
{
    int index;
    access_user_t *user;

    if (state == NULL || uid == NULL) return ACCESS_DENIED_UNKNOWN_CARD;
    index = find_user(state, uid);
    if (index < 0 || !state->users[index].authorized) {
        return ACCESS_DENIED_UNKNOWN_CARD;
    }

    user = &state->users[index];

    /* Safety: permit exit even after the normal schedule closes, so occupants
       are not trapped. The team should confirm emergency-egress expectations
       with the lecturer and document the chosen policy. */
    if (direction == ACCESS_DIR_IN && !schedule_open) {
        return ACCESS_DENIED_OUTSIDE_SCHEDULE;
    }

    if (direction == ACCESS_DIR_IN) {
        if (user->inside) return ACCESS_DENIED_ALREADY_INSIDE;
        if (state->occupancy >= state->capacity) return ACCESS_DENIED_CAPACITY;
        user->inside = true;
        state->occupancy++;
        return ACCESS_GRANTED;
    }

    if (direction == ACCESS_DIR_OUT) {
        if (!user->inside) return ACCESS_DENIED_NOT_INSIDE;
        user->inside = false;
        if (state->occupancy > 0) state->occupancy--;
        return ACCESS_GRANTED;
    }

    return ACCESS_DENIED_UNKNOWN_CARD;
}
