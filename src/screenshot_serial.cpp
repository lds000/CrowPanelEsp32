/**
 * USB-serial screen dump + diagnostics — works on locked-down corporate WiFi.
 *
 * Text commands:
 *   ping            -> quick liveness check
 *   hello / info    -> build + memory summary
 *   diag / status   -> diagnostic summary
 *   capture / c     -> BMP stream with textual progress preamble
 *
 * Binary protocol for capture:
 *   Magic 4 bytes: "LBSC"
 *   uint32_t little-endian: BMP byte count
 *   raw BMP bytes
 *
 * At 115200 baud, ~1.1 MB takes ~90–120 s — use 921600 in Serial Monitor for ~10 s.
 */

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "screenshot_bmp.h"
#include "screenshot_serial.h"

/* Magic must not appear inside BMP (starts with "BM") */
static const char kMagic[] = "LBSC";

static bool streq_ic(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool contains_ic(const char *haystack, const char *needle) {
    if (!*needle) return true;
    for (const char *h = haystack; *h; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b) {
            char ca = *a;
            char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static size_t strnlen_local(const char *s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n]) ++n;
    return n;
}

static const char *app_mode_name() {
#if APP_MODE == APP_MODE_LIVE
    return "LIVE";
#else
    return "DEMO";
#endif
}

static void write_prefixed(Stream &io, const char *label, const char *msg) {
    io.print("[");
    io.print(label);
    io.print("] ");
    io.println(msg);
}

static void write_prefixed_kv(Stream &io, const char *label, const char *key, uint32_t value) {
    io.print("[");
    io.print(label);
    io.print("] ");
    io.print(key);
    io.println(value);
}

static void emit_hello(Stream &io, const char *label) {
    io.print("[");
    io.print(label);
    io.print("] HELLO app=LawnBot-CrowPanel mode=");
    io.print(app_mode_name());
    io.print(" build=");
    io.print(__DATE__);
    io.print("_");
    io.print(__TIME__);
    io.print(" heap=");
    io.print(ESP.getFreeHeap());
    io.print(" psram_free=");
    io.print(ESP.getFreePsram());
    io.print(" psram_size=");
    io.println(ESP.getPsramSize());
}

static void emit_diag(Stream &io, const char *label) {
    io.print("[");
    io.print(label);
    io.print("] DIAG capture=enabled heap=");
    io.print(ESP.getFreeHeap());
    io.print(" psram_free=");
    io.print(ESP.getFreePsram());
    io.print(" psram_size=");
    io.print(ESP.getPsramSize());
    io.print(" uptime_ms=");
    io.println(millis());
}

static void emit_rx_debug(Stream &io, const char *label, const char *line) {
    size_t len = strnlen_local(line, 96);
    io.print("[");
    io.print(label);
    io.print("] RX_DEBUG len=");
    io.print((uint32_t)len);
    io.print(" ascii=\"");
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = (uint8_t)line[i];
        if (ch == '\\') io.print("\\\\");
        else if (ch == '"') io.print("\\\"");
        else if (ch == '\t') io.print("\\t");
        else if (ch >= 32 && ch <= 126) io.write(ch);
        else {
            io.print("\\x");
            if (ch < 16) io.print('0');
            io.print(ch, HEX);
        }
    }
    io.print("\" hex=");
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = (uint8_t)line[i];
        if (i) io.print(' ');
        if (ch < 16) io.print('0');
        io.print(ch, HEX);
    }
    io.println();
}

static void maybe_handle_capture(Stream &io, const char *label, char *line, size_t &pos) {
    while (io.available()) {
        int c = io.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = '\0';
            pos        = 0;
            if (streq_ic(line, "ping") || contains_ic(line, "ping")) {
                io.print("[");
                io.print(label);
                io.print("] PONG uptime_ms=");
                io.println(millis());
                continue;
            }
            if (streq_ic(line, "hello") || streq_ic(line, "info") ||
                contains_ic(line, "hello") || contains_ic(line, "info")) {
                emit_hello(io, label);
                continue;
            }
            if (streq_ic(line, "diag") || streq_ic(line, "status") ||
                contains_ic(line, "diag") || contains_ic(line, "status")) {
                emit_diag(io, label);
                continue;
            }

            bool ok = streq_ic(line, "capture") || streq_ic(line, "c") || contains_ic(line, "capture");
            if (!ok) {
                write_prefixed(io, label, "ERR unknown_command");
                emit_rx_debug(io, label, line);
                continue;
            }

            write_prefixed(io, label, "CAPTURE_ACK command_received");
            write_prefixed_kv(io, label, "CAPTURE_UPTIME_MS=", millis());
            uint8_t *bmp = nullptr;
            uint32_t sz  = 0;
            if (!screenshot_bmp_capture(&bmp, &sz)) {
                write_prefixed(io, label, "CAPTURE_ERROR snapshot_failed");
                continue;
            }

            io.print("[");
            io.print(label);
            io.print("] CAPTURE_READY bytes=");
            io.println(sz);

            io.write((const uint8_t *)kMagic, 4);
            io.write((uint8_t)(sz & 0xFF));
            io.write((uint8_t)((sz >> 8) & 0xFF));
            io.write((uint8_t)((sz >> 16) & 0xFF));
            io.write((uint8_t)((sz >> 24) & 0xFF));

            const uint8_t *p = bmp;
            uint32_t rem      = sz;
            while (rem > 0) {
                size_t chunk = rem > 2048 ? 2048 : rem;
                size_t w     = io.write(p, chunk);
                if (w == 0) {
                    delay(1);
                    continue;
                }
                p += w;
                rem -= w;
                yield();
            }
            screenshot_bmp_free(bmp);
            io.print("\n[");
            io.print(label);
            io.print("] CAPTURE_DONE bytes=");
            io.println(sz);
            continue;
        }
        if (pos < sizeof(line) - 1)
            line[pos++] = (char)c;
        else
            pos = 0;
    }
}

void screenshot_serial_poll() {
    static char line_usb[96];
    static size_t pos_usb = 0;
    static char line_uart[96];
    static size_t pos_uart = 0;

    maybe_handle_capture(Serial, "Serial", line_usb, pos_usb);
    maybe_handle_capture(Serial0, "UART", line_uart, pos_uart);
}
