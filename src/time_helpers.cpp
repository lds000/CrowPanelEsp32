#include "time_helpers.h"

#include <cstdio>
#include <cstring>

namespace lawnbot_time {
namespace {

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool valid_date(int year, int month, int day) {
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1) return false;
    int maximum = DAYS[month - 1];
    if (month == 2 && leap_year(year)) maximum = 29;
    return day <= maximum;
}

bool valid_time(int hour, int minute) {
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
}

}  // namespace

int32_t civil_day_number(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const int adjusted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned doy = (153U * static_cast<unsigned>(adjusted_month) + 2U) / 5U
                       + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + static_cast<int32_t>(doe);
}

int schedule_index_for_civil_date(int year, unsigned month, unsigned day,
                                  int cycle_days) {
    if (cycle_days <= 0 || !valid_date(year, static_cast<int>(month),
                                      static_cast<int>(day))) return -1;
    const int32_t days = civil_day_number(year, month, day)
                       - civil_day_number(2024, 1, 1);
    return static_cast<int>(((days % cycle_days) + cycle_days) % cycle_days);
}

bool parse_next_run_time_text(const char *text,
                              char *time_out, std::size_t time_out_size,
                              char *date_out, std::size_t date_out_size) {
    if (!time_out || time_out_size == 0 || !date_out || date_out_size == 0) return false;
    time_out[0] = '\0';
    date_out[0] = '\0';
    if (!text || !text[0]) return false;

    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if ((std::sscanf(text, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) == 5 ||
         std::sscanf(text, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5) &&
        valid_date(year, month, day) && valid_time(hour, minute)) {
        std::snprintf(time_out, time_out_size, "%02d:%02d", hour, minute);
        std::snprintf(date_out, date_out_size, "%04d-%02d-%02d", year, month, day);
        return true;
    }

    if (std::strlen(text) == 5 && text[0] >= '0' && text[0] <= '9' &&
        text[1] >= '0' && text[1] <= '9' && text[2] == ':' &&
        text[3] >= '0' && text[3] <= '9' && text[4] >= '0' && text[4] <= '9' &&
        std::sscanf(text, "%d:%d", &hour, &minute) == 2 && valid_time(hour, minute)) {
        std::snprintf(time_out, time_out_size, "%02d:%02d", hour, minute);
        return true;
    }
    return false;
}

}  // namespace lawnbot_time
