/*
 * Slot writer shared by browser uploads and GitHub downloads. Streams an
 * app-only image into ota_<slot> with esp_ota_begin/write/end (never touching
 * otadata), and refuses obviously wrong files BEFORE erasing anything.
 *
 * Rule: after ANY non-OK return, or when the caller rejects the result, call
 * sw_abort(); it blanks the slot if (and only if) its old content was erased.
 * After a fully accepted install call sw_done().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_app_desc.h"

typedef struct {
    esp_app_desc_t desc;
    uint32_t size;
    uint8_t sha256[32];     /* SHA-256 of every byte received (compare with GitHub's "digest") */
    bool marker_found;      /* the required marker string was in the image (true if none required) */
    bool display_seen;      /* image mentions CO5300 (this board's display driver) */
} sw_result_t;

esp_err_t sw_begin(int slot, uint32_t expected_size, const char *required_marker, char *err, size_t errlen);
esp_err_t sw_write(const uint8_t *data, size_t len, char *err, size_t errlen);
esp_err_t sw_finish(sw_result_t *res, char *err, size_t errlen);
void sw_abort(void);
void sw_done(void);
bool sw_erased(void);       /* has the slot's previous content been destroyed? */
uint32_t sw_received(void);
