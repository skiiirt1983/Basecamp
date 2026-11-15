#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "apps_logic.h"

typedef enum {
    SLOT_EMPTY = 0,     /* nothing bootable (a blank slot, or an install that failed) */
    SLOT_APP,           /* valid app image */
    SLOT_DAMAGED,       /* image present but failed verification / wrong board */
} slot_state_t;

typedef struct {
    const esp_partition_t *part;
    slot_state_t state;
    bool changed;           /* image differs from the baseline recorded after install (overwrite guard) */
    bool has_src;           /* a GitHub source is stored: can be updated / reinstalled */
    bool generic_desc;      /* app descriptor says nothing useful (Arduino builds) */
    esp_app_desc_t desc;
    char name[32];          /* name shown on the card */
    char version[40];
    char detail[48];        /* why a card is grey */
    char tag[32];           /* GitHub release tag installed by the launcher ("" if unknown) */
    uint32_t color;         /* 0xRRGGBB */
    uint32_t image_len;
} basecamp_slot_t;

void slots_init(void);
void slots_scan_all(void);
void slots_scan_one(int slot);
void slots_get(int slot, basecamp_slot_t *out);
void slots_get_all(basecamp_slot_t out[APPS_MAX]);
bool slots_has_card(const basecamp_slot_t *s);   /* shown as a card on the menu */
unsigned slots_card_mask(void);
int slots_first_free(void);                      /* first SLOT_EMPTY slot, or -1 */

/* Accept the current image of a slot as the overwrite guard's baseline. */
void slots_accept(int slot);
/* Blank a slot, forget its metadata and rescan it. */
esp_err_t slots_erase(int slot);
