#pragma once
#include <stdbool.h>
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "basecamp_bootsel.h"

typedef struct {
    const esp_partition_t *part;  /* NULL if the slot is missing from the table */
    bool valid;                   /* bootable image present */
    bool generic_desc;            /* app descriptor is not app-specific (Arduino) */
    bool changed;                 /* image differs from the baseline recorded after flashing */
    esp_app_desc_t desc;
    char name[32];                /* label to show */
    char version[40];             /* version to show */
    char detail[48];              /* small text: why greyed out / project name */
} basecamp_slot_t;

void slots_scan(basecamp_slot_t slots[BASECAMP_NUM_SLOTS]);

/* Accept the current image of a slot as the new baseline (clears "changed"). */
void slots_accept(basecamp_slot_t *slot, int index);
