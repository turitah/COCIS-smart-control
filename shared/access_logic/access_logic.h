#ifndef ACCESS_LOGIC_H
#define ACCESS_LOGIC_H

#include <stdbool.h>
#include <stddef.h>

#define ACCESS_UID_MAX_LEN 16
#define ACCESS_MAX_USERS 8

typedef enum {
    ACCESS_DIR_IN = 0,
    ACCESS_DIR_OUT = 1
} access_direction_t;

typedef enum {
    ACCESS_GRANTED = 0,
    ACCESS_DENIED_UNKNOWN_CARD,
    ACCESS_DENIED_OUTSIDE_SCHEDULE,
    ACCESS_DENIED_ALREADY_INSIDE,
    ACCESS_DENIED_NOT_INSIDE,
    ACCESS_DENIED_CAPACITY
} access_result_t;

typedef struct {
    char uid[ACCESS_UID_MAX_LEN + 1];
    bool authorized;
    bool inside;
} access_user_t;

typedef struct {
    access_user_t users[ACCESS_MAX_USERS];
    size_t count;
    size_t occupancy;
    size_t capacity;
} access_state_t;

void access_state_init(access_state_t *state, size_t capacity);
bool access_register_user(access_state_t *state, const char *uid);
access_result_t access_process_card(access_state_t *state,
                                    const char *uid,
                                    access_direction_t direction,
                                    bool schedule_open);

#endif
