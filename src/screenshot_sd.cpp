#include "screenshot_sd.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <stdio.h>

#include "screenshot_bmp.h"

namespace {
constexpr int SD_CS   = 10;
constexpr int SD_MOSI = 11;
constexpr int SD_CLK  = 12;
constexpr int SD_MISO = 13;

SPIClass g_sd_spi(FSPI);
bool g_sd_ready = false;
TaskHandle_t g_sd_save_task = nullptr;

enum SaveState : uint8_t {
    SAVE_IDLE,
    SAVE_WRITING,
    SAVE_DONE_OK,
    SAVE_DONE_FAIL
};

volatile SaveState g_save_state = SAVE_IDLE;
uint8_t *g_save_bmp = nullptr;
uint32_t g_save_bmp_size = 0;
char g_save_path[48] = {};

bool ensure_capture_dir() {
    if (SD.exists("/captures")) return true;
    return SD.mkdir("/captures");
}

bool next_capture_path(char *out_path, size_t out_len) {
    for (int i = 1; i <= 9999; ++i) {
        snprintf(out_path, out_len, "/captures/crowpanel_%04d.bmp", i);
        if (!SD.exists(out_path)) return true;
    }
    return false;
}

void finish_save(bool ok) {
    if (g_save_bmp) {
        screenshot_bmp_free(g_save_bmp);
        g_save_bmp = nullptr;
    }
    g_save_bmp_size = 0;
    g_sd_save_task = nullptr;
    g_save_state = ok ? SAVE_DONE_OK : SAVE_DONE_FAIL;
}

void screenshot_sd_writer_task(void * /*param*/) {
    bool ok = false;

    File f = SD.open(g_save_path, FILE_WRITE);
    if (f) {
        const size_t chunk_size = 4096;
        size_t offset = 0;
        ok = true;
        while (offset < g_save_bmp_size) {
            const size_t to_write = min(chunk_size, (size_t)(g_save_bmp_size - offset));
            if (f.write(g_save_bmp + offset, to_write) != to_write) {
                ok = false;
                break;
            }
            offset += to_write;
            delay(1);
        }
        f.flush();
        f.close();
    }

    if (!ok) SD.remove(g_save_path);
    finish_save(ok);
    vTaskDelete(nullptr);
}
}  // namespace

bool screenshot_sd_init() {
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    g_sd_spi.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    g_sd_ready = SD.begin(SD_CS, g_sd_spi, 10000000);
    if (!g_sd_ready) return false;
    return ensure_capture_dir();
}

bool screenshot_sd_ready() {
    return g_sd_ready;
}

bool screenshot_sd_is_busy() {
    return g_save_state == SAVE_WRITING;
}

bool screenshot_sd_start_save_latest() {
    if (g_save_state == SAVE_WRITING) return false;
    if (!g_sd_ready && !screenshot_sd_init()) return false;
    if (!ensure_capture_dir()) return false;
    if (!next_capture_path(g_save_path, sizeof(g_save_path))) return false;

    if (!screenshot_bmp_capture(&g_save_bmp, &g_save_bmp_size)) return false;

    g_save_state = SAVE_WRITING;
    BaseType_t task_ok = xTaskCreate(
        screenshot_sd_writer_task,
        "snap_sd",
        6144,
        nullptr,
        1,
        &g_sd_save_task
    );
    if (task_ok != pdPASS) {
        finish_save(false);
        g_save_state = SAVE_IDLE;
        return false;
    }
    return true;
}

bool screenshot_sd_poll_result(bool *out_success, char *out_path, size_t out_path_len) {
    if (g_save_state != SAVE_DONE_OK && g_save_state != SAVE_DONE_FAIL) return false;
    const bool ok = (g_save_state == SAVE_DONE_OK);
    if (out_success) *out_success = ok;
    if (out_path && out_path_len) {
        snprintf(out_path, out_path_len, "%s", g_save_path);
    }
    g_save_path[0] = '\0';
    g_save_state = SAVE_IDLE;
    return true;
}
