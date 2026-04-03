/*
 * SaveB — Save cross-pollution test (step=2))
 *
 * Tests save integrity across app loads. Uses 10 virtual sub-saves
 * (32KB each) within save slots 0-4. Each run:
 *   1. Verify all 10 sub-saves have correct CRC (detect corruption)
 *   2. Rotate: move sub-save at position[i] to position[(i+STEP)%10]
 *   3. Write back with updated iteration counter
 *
 * Run alternately with SaveB (step=2) to detect cross-pollution.
 */

#define APP_ID    0xBB
#define APP_NAME  "SaveB"
#define STEP      2

#include "of.h"
#include <stdio.h>
#include <string.h>

#define NUM_VSAVES      10
#define VSAVE_SIZE_EVEN 32768   /* 32KB for even slots (0,2,4,6,8) */
#define VSAVE_SIZE_ODD  8192    /* 8KB for odd slots (1,3,5,7,9) */
#define VSAVE_SIZE      VSAVE_SIZE_EVEN  /* max, for buffer sizing */
#define SLOT_SIZE        VSAVE_SIZE_EVEN /* 1 vsave per slot */
#define MAGIC           0x5356  /* "SV" */

/* Each virtual save layout:
 *   [0..3]   magic (0x5356) + app_id + slot_index
 *   [4..7]   iteration counter (increments each rotation)
 *   [8..11]  CRC32 of bytes [16..VSAVE_SIZE-1]
 *   [12..15] reserved (0)
 *   [16..VSAVE_SIZE-1]  random data seeded from (app_id, slot_index, iteration)
 */

typedef struct {
    uint16_t magic;
    uint8_t  app_id;
    uint8_t  slot_index;
    uint32_t iteration;
    uint32_t crc;
    uint32_t reserved;
} vsave_header_t;

#define PAYLOAD_OFFSET  16

static uint32_t vsave_data_size(int idx) {
    return (idx & 1) ? VSAVE_SIZE_ODD : VSAVE_SIZE_EVEN;
}
#define PAYLOAD_SIZE(sz) ((sz) - PAYLOAD_OFFSET)

/* Simple CRC32 (no table, small code) */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* Deterministic PRNG seeded from app_id + slot + iteration */
static uint32_t xorshift_state;
static void seed_rng(uint8_t app_id, uint8_t slot, uint32_t iter) {
    xorshift_state = ((uint32_t)app_id << 24) ^ ((uint32_t)slot << 16) ^ iter ^ 0x12345678;
    if (xorshift_state == 0) xorshift_state = 1;
}
static uint32_t xorshift32(void) {
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

/* Number of physical slots used by NUM_VSAVES virtual saves */
#define NUM_SLOTS       ((NUM_VSAVES + (SLOT_SIZE / VSAVE_SIZE) - 1) / (SLOT_SIZE / VSAVE_SIZE))

static void flush_all_slots(void) {
    for (int s = 0; s < NUM_SLOTS; s++)
        of_save_flush(s);
}

/* Map virtual save index to (slot, offset) */
static void vsave_location(int vsave_idx, int *slot, uint32_t *offset) {
    /* 10 vsaves × 32KB = 320KB. Slots are 128KB each, so 4 vsaves per slot.
     * vsave 0-3 → slot 0; vsave 4-7 → slot 1; vsave 8-9 → slot 2 */
    int vsaves_per_slot = SLOT_SIZE / VSAVE_SIZE;
    *slot = vsave_idx / vsaves_per_slot;
    *offset = (vsave_idx % vsaves_per_slot) * VSAVE_SIZE;
}

static uint8_t buf[VSAVE_SIZE];

static void generate_payload(uint8_t app_id, uint8_t slot_index,
                             uint32_t iteration, uint32_t payload_size) {
    seed_rng(app_id, slot_index, iteration);
    for (uint32_t i = 0; i < payload_size; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = payload_size - i;
        if (remaining >= 4) {
            buf[PAYLOAD_OFFSET + i]     = (uint8_t)(r);
            buf[PAYLOAD_OFFSET + i + 1] = (uint8_t)(r >> 8);
            buf[PAYLOAD_OFFSET + i + 2] = (uint8_t)(r >> 16);
            buf[PAYLOAD_OFFSET + i + 3] = (uint8_t)(r >> 24);
        } else {
            for (uint32_t j = 0; j < remaining; j++)
                buf[PAYLOAD_OFFSET + i + j] = (uint8_t)(r >> (j * 8));
        }
    }
}

static int write_vsave(int vsave_idx, uint32_t iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    /* Fill header */
    vsave_header_t *hdr = (vsave_header_t *)buf;
    hdr->magic = MAGIC;
    hdr->app_id = APP_ID;
    hdr->slot_index = (uint8_t)vsave_idx;
    hdr->iteration = iteration;
    hdr->reserved = 0;

    /* Generate deterministic payload */
    generate_payload(APP_ID, (uint8_t)vsave_idx, iteration, psz);

    /* Compute CRC over payload */
    hdr->crc = crc32(&buf[PAYLOAD_OFFSET], psz);

    /* Write to save and flush with this vsave's size */
    int rc = of_save_write(slot, buf, offset, sz);
    if (rc != (int)sz) return -1;
    of_save_flush_size(slot, sz);
    return 0;
}

static int verify_vsave(int vsave_idx, uint32_t *out_iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    /* Read from save */
    int rc = of_save_read(slot, buf, offset, sz);
    if (rc != (int)sz) return -1;

    vsave_header_t *hdr = (vsave_header_t *)buf;

    /* Check magic */
    if (hdr->magic != MAGIC) return -2;

    /* Check app_id — detects cross-pollution */
    if (hdr->app_id != APP_ID) return -3;

    /* Check slot_index */
    if (hdr->slot_index != (uint8_t)vsave_idx) return -4;

    /* Verify CRC of payload data */
    uint32_t stored_crc = hdr->crc;
    uint32_t actual_crc = crc32(&buf[PAYLOAD_OFFSET], psz);
    if (actual_crc != stored_crc) return -5;

    /* Verify payload content matches expected PRNG sequence */
    uint32_t iter = hdr->iteration;
    seed_rng(APP_ID, (uint8_t)vsave_idx, iter);
    for (uint32_t i = 0; i < psz; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = psz - i;
        uint32_t check = (remaining >= 4) ? 4 : remaining;
        for (uint32_t j = 0; j < check; j++) {
            if (buf[PAYLOAD_OFFSET + i + j] != (uint8_t)(r >> (j * 8)))
                return -6;
        }
    }

    if (out_iteration) *out_iteration = iter;
    return 0;
}

static int is_virgin(void) {
    uint8_t hdr[4];
    of_save_read(0, hdr, 0, 4);
    uint16_t magic = hdr[0] | (hdr[1] << 8);
    uint8_t app_id = hdr[2];
    return (magic != MAGIC || app_id != APP_ID);
}

int main(void) {
    printf("\033[2J\033[H");
    printf("\033[93m  %s Save Test (step=%d)\033[0m\n\n", APP_NAME, STEP);

    /* Raw byte test: read first 32 bytes via API */
    {
        uint8_t raw[32];
        of_save_read(0, raw, 0, 32);
        printf("  Bytes @0: ");
        for (int i = 0; i < 32; i++) printf("%02X ", raw[i]);
        printf("\n\n");
    }

    if (is_virgin()) {
        /* First run: initialize all 10 virtual saves */
        printf("  First run — initializing %d virtual saves...\n", NUM_VSAVES);
        for (int i = 0; i < NUM_VSAVES; i++) {
            int rc = write_vsave(i, 0);
            if (rc < 0) {
                printf("  \033[91mFAIL\033[0m write vsave %d (rc=%d)\n", i, rc);
                goto done;
            }
            printf("  [%d] init ok\n", i);
        }
        printf("\n  \033[92mInitialized. Run again to test rotation.\033[0m\n");
    } else {
        /* Subsequent run: verify all, then rotate */
        printf("  Verifying all %d virtual saves...\n", NUM_VSAVES);

        uint32_t iterations[NUM_VSAVES];
        int all_ok = 1;

        for (int i = 0; i < NUM_VSAVES; i++) {
            int rc = verify_vsave(i, &iterations[i]);
            if (rc < 0) {
                all_ok = 0;
                const char *reason;
                switch (rc) {
                    case -2: reason = "bad magic"; break;
                    case -3: reason = "WRONG APP_ID (cross-pollution!)"; break;
                    case -4: reason = "wrong slot_index"; break;
                    case -5: reason = "CRC MISMATCH (data corruption!)"; break;
                    case -6: reason = "PAYLOAD MISMATCH (bit rot!)"; break;
                    default: reason = "read error"; break;
                }
                printf("  \033[91mFAIL\033[0m vsave %d: %s\n", i, reason);
            } else {
                printf("  [%d] ok (iter=%d)\n", i, (int)iterations[i]);
            }
        }

        if (!all_ok) {
            printf("\n  \033[91mVERIFICATION FAILED\033[0m\n");
            goto done;
        }

        printf("\n  Rotating by step=%d...\n", STEP);

        /* Read all vsaves into temp storage, then write to rotated positions.
         * We need to be careful about order to avoid overwriting before reading.
         * Simple approach: read all first, then write all. */

        /* We only have one buf, so we do it one at a time using the
         * iteration as the content generator (no need to buffer all 10). */

        /* Compute new iterations: each gets previous iteration + 1 */
        uint32_t new_iters[NUM_VSAVES];
        for (int i = 0; i < NUM_VSAVES; i++) {
            int src = ((i - STEP) % NUM_VSAVES + NUM_VSAVES) % NUM_VSAVES;
            new_iters[i] = iterations[src] + 1;
        }

        /* Write rotated saves */
        for (int i = 0; i < NUM_VSAVES; i++) {
            int rc = write_vsave(i, new_iters[i]);
            if (rc < 0) {
                printf("  \033[91mFAIL\033[0m write vsave %d\n", i);
                goto done;
            }
        }

        /* Verify the writes */
        int verify_ok = 1;
        for (int i = 0; i < NUM_VSAVES; i++) {
            int rc = verify_vsave(i, NULL);
            if (rc < 0) {
                verify_ok = 0;
                printf("  \033[91mFAIL\033[0m post-rotate verify vsave %d (rc=%d)\n", i, rc);
            }
        }

        if (verify_ok)
            printf("\n  \033[92mPASS\033[0m — all %d vsaves rotated and verified\n", NUM_VSAVES);
        else
            printf("\n  \033[91mFAIL\033[0m — post-rotate verification failed\n");
    }

done:
    while (1)
        of_delay_ms(100);
    return 0;
}
