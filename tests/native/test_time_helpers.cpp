#include "time_helpers.h"

#include <cassert>
#include <cstring>

using lawnbot_time::parse_next_run_time_text;
using lawnbot_time::schedule_index_for_civil_date;

int main() {
    assert(schedule_index_for_civil_date(2024, 1, 1) == 0);
    assert(schedule_index_for_civil_date(2023, 12, 31) == 13);
    assert(schedule_index_for_civil_date(2024, 1, 15) == 0);

    /* Mountain-time DST boundaries still advance exactly one civil day. */
    assert(schedule_index_for_civil_date(2024, 3, 10) ==
           (schedule_index_for_civil_date(2024, 3, 9) + 1) % 14);
    assert(schedule_index_for_civil_date(2024, 3, 11) ==
           (schedule_index_for_civil_date(2024, 3, 10) + 1) % 14);
    assert(schedule_index_for_civil_date(2024, 11, 3) ==
           (schedule_index_for_civil_date(2024, 11, 2) + 1) % 14);
    assert(schedule_index_for_civil_date(2024, 11, 4) ==
           (schedule_index_for_civil_date(2024, 11, 3) + 1) % 14);
    assert(schedule_index_for_civil_date(2024, 2, 30) == -1);

    char time[8] = {};
    char date[12] = {};
    assert(parse_next_run_time_text("2026-07-17T06:35:00-06:00",
                                    time, sizeof(time), date, sizeof(date)));
    assert(std::strcmp(time, "06:35") == 0);
    assert(std::strcmp(date, "2026-07-17") == 0);

    assert(parse_next_run_time_text("2026-11-01 23:59", time, sizeof(time),
                                    date, sizeof(date)));
    assert(std::strcmp(time, "23:59") == 0);
    assert(std::strcmp(date, "2026-11-01") == 0);

    assert(parse_next_run_time_text("07:05", time, sizeof(time), date, sizeof(date)));
    assert(std::strcmp(time, "07:05") == 0);
    assert(date[0] == '\0');

    assert(!parse_next_run_time_text("2026-02-30T06:00", time, sizeof(time),
                                     date, sizeof(date)));
    assert(!parse_next_run_time_text("25:00", time, sizeof(time), date, sizeof(date)));
    assert(!parse_next_run_time_text("07:05garbage", time, sizeof(time),
                                     date, sizeof(date)));
    assert(time[0] == '\0' && date[0] == '\0');
    return 0;
}
