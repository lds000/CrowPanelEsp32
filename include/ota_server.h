#pragma once

/**
 * OTA update server — ArduinoOTA (PlatformIO espota) + HTTP browser upload.
 * Call ota_server_init() once after WiFi connects.
 * Call ota_server_loop() every loop iteration.
 */
void ota_server_init();
void ota_server_loop();
