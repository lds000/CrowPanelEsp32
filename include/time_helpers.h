#pragma once

#include <cstddef>
#include <cstdint>

namespace lawnbot_time {

/* Proleptic Gregorian civil-day ordinal.  It intentionally has no timezone
 * or Arduino dependency so schedule math can be exercised on a host. */
int32_t civil_day_number(int year, unsigned month, unsigned day);

/* Return a non-negative rotating schedule index anchored to 2024-01-01. */
int schedule_index_for_civil_date(int year, unsigned month, unsigned day,
                                  int cycle_days = 14);

/* Parse either an ISO-like date/time or HH:MM into fixed display buffers.
 * Returns false and clears both buffers for malformed/out-of-range input. */
bool parse_next_run_time_text(const char *text,
                              char *time_out, std::size_t time_out_size,
                              char *date_out, std::size_t date_out_size);

}  // namespace lawnbot_time
