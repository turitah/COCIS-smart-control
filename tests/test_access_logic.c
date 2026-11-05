#include "../shared/access_logic/access_logic.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    access_state_t s;
    access_state_init(&s, 2);

    assert(access_register_user(&s, "A1B2"));
    assert(access_register_user(&s, "C3D4"));
    assert(access_register_user(&s, "E5F6"));
    assert(access_register_user(&s, "A1B2")); /* duplicate registration is safe */
    assert(access_register_user(&s, "00112233445566778899")); /* 10-byte UID */
    assert(!access_register_user(&s, "001122334455667788990"));
    assert(s.count == 4);
    assert(s.users[3].uid[ACCESS_UID_MAX_LEN] == '\0');

    assert(access_process_card(&s, "ZZ99", ACCESS_DIR_IN, true) ==
           ACCESS_DENIED_UNKNOWN_CARD);
    assert(s.occupancy == 0);

    assert(access_process_card(&s, "00112233445566778899", ACCESS_DIR_IN,
                               true) == ACCESS_GRANTED);
    assert(s.occupancy == 1);
    assert(access_process_card(&s, "00112233445566778899", ACCESS_DIR_OUT,
                               true) == ACCESS_GRANTED);
    assert(s.occupancy == 0);

    assert(access_process_card(&s, "A1B2", (access_direction_t)2, true) ==
           ACCESS_DENIED_UNKNOWN_CARD);
    assert(s.occupancy == 0);

    assert(access_process_card(&s, "A1B2", ACCESS_DIR_IN, false) ==
           ACCESS_DENIED_OUTSIDE_SCHEDULE);
    assert(s.occupancy == 0);

    assert(access_process_card(&s, "A1B2", ACCESS_DIR_IN, true) == ACCESS_GRANTED);
    assert(s.occupancy == 1);

    assert(access_process_card(&s, "A1B2", ACCESS_DIR_IN, true) ==
           ACCESS_DENIED_ALREADY_INSIDE);
    assert(s.occupancy == 1);

    assert(access_process_card(&s, "C3D4", ACCESS_DIR_IN, true) == ACCESS_GRANTED);
    assert(s.occupancy == 2);

    assert(access_process_card(&s, "E5F6", ACCESS_DIR_IN, true) ==
           ACCESS_DENIED_CAPACITY);
    assert(s.occupancy == 2);

    /* Exit remains permitted after schedule closes for safe egress. */
    assert(access_process_card(&s, "A1B2", ACCESS_DIR_OUT, false) == ACCESS_GRANTED);
    assert(s.occupancy == 1);

    assert(access_process_card(&s, "A1B2", ACCESS_DIR_OUT, true) ==
           ACCESS_DENIED_NOT_INSIDE);
    assert(s.occupancy == 1);

    assert(access_schedule_is_open(7 * 60, 18 * 60, 8 * 60));
    assert(access_schedule_is_open(7 * 60, 18 * 60, 7 * 60));
    assert(!access_schedule_is_open(7 * 60, 18 * 60, 18 * 60));
    assert(!access_schedule_is_open(7 * 60, 18 * 60, 6 * 60 + 59));
    assert(!access_schedule_is_open(7 * 60, 18 * 60, 18 * 60 + 1));

    assert(access_schedule_is_open(22 * 60, 6 * 60, 23 * 60));
    assert(access_schedule_is_open(22 * 60, 6 * 60, 0));
    assert(access_schedule_is_open(22 * 60, 6 * 60, 5 * 60 + 59));
    assert(!access_schedule_is_open(22 * 60, 6 * 60, 6 * 60));
    assert(!access_schedule_is_open(22 * 60, 6 * 60, 12 * 60));

    assert(access_schedule_is_open(0, 0, 0));
    assert(access_schedule_is_open(0, 0, 12 * 60));
    assert(!access_schedule_is_open(-1, 18 * 60, 8 * 60));
    assert(!access_schedule_is_open(7 * 60, 24 * 60, 8 * 60));
    assert(!access_schedule_is_open(7 * 60, 18 * 60, 24 * 60));

    puts("PASS: access logic tests completed successfully.");
    return 0;
}
