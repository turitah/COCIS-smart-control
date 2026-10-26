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

    assert(access_process_card(&s, "ZZ99", ACCESS_DIR_IN, true) ==
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

    puts("PASS: access logic tests completed successfully.");
    return 0;
}
