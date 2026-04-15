/*
 * of_smp_bank.c -- .ofsf runtime bank reader
 *
 * The kernel auto-loads the first .ofsf it finds in a data slot at boot
 * and hands the buffer to apps via OF_SVC->smp_bank_preload_base. This
 * file just binds our static preset/zone/sample pointers to that buffer
 * inside an SDK constructor, so apps can call of_smp_zone_lookup and
 * of_smp_bank_get without any explicit init step.
 */

#include "include/of_smp_bank.h"
#include "include/of_services.h"

#include <stdint.h>

static const ofsf_header_t *loaded_header;
static const ofsf_preset_t *loaded_presets;
static const ofsf_zone_t   *loaded_zones;
static const void           *sample_base;   /* absolute CRAM1 address of sample blob */

/* Priority 102: runs after of_init.c (101) has captured OF_SVC from auxv,
 * but before any app constructor or main(). */
__attribute__((constructor(102)))
static void bank_autobind(void)
{
    const void *buf = OF_SVC->smp_bank_preload_base;
    if (!buf) return;

    const ofsf_header_t *hdr = (const ofsf_header_t *)buf;
    if (hdr->magic != OFSF_MAGIC || hdr->version != OFSF_VERSION)
        return;

    const uint8_t *base = (const uint8_t *)buf;
    loaded_header  = hdr;
    loaded_presets = (const ofsf_preset_t *)(base + sizeof(ofsf_header_t));
    loaded_zones   = (const ofsf_zone_t *)(base + sizeof(ofsf_header_t) +
                      OFSF_PRESET_COUNT * sizeof(ofsf_preset_t));
    sample_base    = base + hdr->sample_data_offset;
}

const ofsf_header_t *of_smp_bank_get(void)
{
    return loaded_header;
}

const void *of_smp_bank_sample_base(void)
{
    return sample_base;
}

int of_smp_zone_lookup(int bank, int program, int key, int velocity,
                       const ofsf_zone_t **zones_out, int max_zones)
{
    if (!loaded_header || !loaded_presets || !loaded_zones)
        return 0;
    if (program < 0 || program > 127)
        return 0;

    /* Preset index: bank 0 = slots 0..127, bank 128 (drums) = slots 128..255 */
    int idx;
    if (bank == 128)
        idx = 128 + program;
    else
        idx = program; /* bank 0 only; other banks not yet supported */

    if (idx < 0 || idx >= OFSF_PRESET_COUNT)
        return 0;

    const ofsf_preset_t *pr = &loaded_presets[idx];
    if (pr->zone_count == 0)
        return 0;

    int found = 0;
    for (int i = 0; i < pr->zone_count && found < max_zones; i++) {
        int zi = pr->zone_start + i;
        if ((uint32_t)zi >= loaded_header->zone_count)
            break;
        const ofsf_zone_t *z = &loaded_zones[zi];
        if (key >= z->key_lo && key <= z->key_hi &&
            velocity >= z->vel_lo && velocity <= z->vel_hi) {
            zones_out[found++] = z;
        }
    }
    return found;
}
