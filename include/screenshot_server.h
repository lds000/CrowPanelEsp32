#pragma once
#include "config.h"

#if ENABLE_SCREENSHOT_HTTP
void screenshot_server_init();
void screenshot_server_loop();
#endif
