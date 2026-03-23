#pragma once

#include <stdbool.h>
#include <stddef.h>

bool screenshot_sd_init();
bool screenshot_sd_ready();
bool screenshot_sd_start_save_latest();
bool screenshot_sd_is_busy();
bool screenshot_sd_poll_result(bool *out_success, char *out_path, size_t out_path_len);
