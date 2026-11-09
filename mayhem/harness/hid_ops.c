/*
 * Fuzz harness for ZMK's HID report builder (app/src/hid.c).
 *
 * The input is read as 3-byte operations that drive the public zmk_hid_* API the way the behaviors
 * do: key/consumer usages are toggled (a held usage is released, an absent one pressed), modifiers
 * are registered/unregistered only while their count is positive, and implicit/masked modifiers are
 * set and cleared. An independent model of what is held is kept next to the real report state, and
 * after every operation the keyboard report, consumer report, modifier byte and USB boot report must
 * agree with it. A mismatch aborts. All state is unwound at the end of each input, so one input's
 * result does not depend on another.
 *
 * Built twice: NKRO (bitmap keyboard report) and HKRO (10-slot keyboard report) via -include of
 * mayhem/shim/config_{nkro,hkro}.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/hid.h>

#define NUM_MODS 8
#define KB_CODES (ZMK_HID_KEYBOARD_NKRO_MAX_USAGE + 1)
#define CONSUMER_CODES (ZMK_HID_CONSUMER_MAX_USAGE + 1)

#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)
#define KB_CAPACITY 100000
#else
#define KB_CAPACITY CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE
#endif
#define CONSUMER_CAPACITY CONFIG_ZMK_HID_CONSUMER_REPORT_SIZE

enum { ABSENT = 0, HELD = 1, DROPPED = 2 };

static uint8_t kb_state[KB_CODES];
static uint8_t consumer_state[CONSUMER_CODES];
static int kb_held, kb_dropped, consumer_held, consumer_dropped;
static int mod_count[NUM_MODS];
static uint8_t implicit_mods, masked_mods;

static void fail(const char *what) {
    fprintf(stderr, "hid model mismatch: %s\n", what);
    abort();
}

static uint8_t explicit_mods(void) {
    uint8_t m = 0;
    for (int i = 0; i < NUM_MODS; i++) {
        if (mod_count[i] > 0) {
            m |= 1 << i;
        }
    }
    return m;
}

static void check_keyboard_report(void) {
    const struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();
    if (r->body.modifiers != (uint8_t)((explicit_mods() & ~masked_mods) | implicit_mods)) {
        fail("modifier byte");
    }
    if (zmk_hid_get_explicit_mods() != explicit_mods()) {
        fail("explicit modifiers");
    }

    int seen = 0;
#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)
    for (unsigned code = 0; code < KB_CODES; code++) {
        if (r->body.keys[code / 8] & (1U << (code % 8))) {
            if (kb_state[code] != HELD) {
                fail("nkro bit set for a key that is not held");
            }
            seen++;
        }
    }
#else
    for (unsigned i = 0; i < sizeof(r->body.keys); i++) {
        uint8_t k = r->body.keys[i];
        if (k == 0) {
            continue;
        }
        if (k >= KB_CODES || kb_state[k] != HELD) {
            fail("hkro slot holds a key that is not held");
        }
        seen++;
    }
#endif
    if (seen != kb_held) {
        fail("number of keys in keyboard report");
    }
}

static void check_consumer_report(void) {
    const struct zmk_hid_consumer_report *r = zmk_hid_get_consumer_report();
    int seen = 0;
    for (unsigned i = 0; i < CONSUMER_CAPACITY; i++) {
        unsigned k = r->body.keys[i];
        if (k == 0) {
            continue;
        }
        if (k >= CONSUMER_CODES || consumer_state[k] != HELD) {
            fail("consumer slot holds a usage that is not held");
        }
        seen++;
    }
    if (seen != consumer_held) {
        fail("number of usages in consumer report");
    }
}

static void check_boot_report(void) {
    const zmk_hid_boot_report_t *b = zmk_hid_get_boot_report();
    const struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();
    if (b->modifiers != r->body.modifiers) {
        fail("boot report modifiers");
    }
    if (kb_held + kb_dropped > HID_BOOT_KEY_LEN) {
        for (int i = 0; i < HID_BOOT_KEY_LEN; i++) {
            if (b->keys[i] != HID_ERROR_ROLLOVER) {
                fail("boot report is not a rollover error");
            }
        }
        return;
    }
    for (int i = 0; i < HID_BOOT_KEY_LEN; i++) {
        uint8_t k = b->keys[i];
        if (i < kb_held) {
            if (k == 0 || k >= KB_CODES || kb_state[k] != HELD) {
                fail("boot report key is not held");
            }
            for (int j = 0; j < i; j++) {
                if (b->keys[j] == k) {
                    fail("boot report repeats a key");
                }
            }
        } else if (k != 0) {
            fail("boot report has extra keys");
        }
    }
}

static void check_all(void) {
    check_keyboard_report();
    check_consumer_report();
    check_boot_report();
    for (unsigned code = 1; code < KB_CODES; code++) {
        if (zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, code)) != (kb_state[code] == HELD)) {
            fail("keyboard is_pressed");
        }
    }
    for (int i = 0; i < NUM_MODS; i++) {
        if (zmk_hid_is_pressed(ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_LEFTCONTROL + i)) !=
            (mod_count[i] > 0)) {
            fail("modifier is_pressed");
        }
    }
}

static unsigned kb_code_for(unsigned idx) {
    idx %= KB_CODES - 1 + NUM_MODS;
    return idx < KB_CODES - 1 ? idx + 1 : HID_USAGE_KEY_KEYBOARD_LEFTCONTROL + (idx - (KB_CODES - 1));
}

static void keyboard_toggle(unsigned code, int generic) {
    uint32_t usage = ZMK_HID_USAGE(HID_USAGE_KEY, code);
    int is_mod_code = code >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL && code <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI;

    if (is_mod_code) {
        int i = code - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL;
        if (mod_count[i] == 0) {
            generic ? zmk_hid_press(usage) : zmk_hid_keyboard_press(code);
            mod_count[i]++;
        } else {
            generic ? zmk_hid_release(usage) : zmk_hid_keyboard_release(code);
            mod_count[i]--;
        }
        return;
    }

    if (kb_state[code] == ABSENT) {
        generic ? zmk_hid_press(usage) : zmk_hid_keyboard_press(code);
        if (kb_held < KB_CAPACITY) {
            kb_state[code] = HELD;
            kb_held++;
        } else {
            kb_state[code] = DROPPED;
            kb_dropped++;
        }
    } else {
        generic ? zmk_hid_release(usage) : zmk_hid_keyboard_release(code);
        if (kb_state[code] == HELD) {
            kb_held--;
        } else {
            kb_dropped--;
        }
        kb_state[code] = ABSENT;
    }
}

static void consumer_toggle(unsigned code, int generic) {
    uint32_t usage = ZMK_HID_USAGE(HID_USAGE_CONSUMER, code);
    if (consumer_state[code] == ABSENT) {
        generic ? zmk_hid_press(usage) : zmk_hid_consumer_press(code);
        if (consumer_held < CONSUMER_CAPACITY) {
            consumer_state[code] = HELD;
            consumer_held++;
        } else {
            consumer_state[code] = DROPPED;
            consumer_dropped++;
        }
    } else {
        generic ? zmk_hid_release(usage) : zmk_hid_consumer_release(code);
        if (consumer_state[code] == HELD) {
            consumer_held--;
        } else {
            consumer_dropped--;
        }
        consumer_state[code] = ABSENT;
    }
}

static void apply(uint8_t kind, uint16_t arg) {
    int generic = kind >> 7;
    switch (kind % 12) {
    case 0:
    case 1:
        keyboard_toggle(kb_code_for(arg), generic);
        break;
    case 2:
        consumer_toggle(1 + arg % ZMK_HID_CONSUMER_MAX_USAGE, generic);
        break;
    case 3:
        zmk_hid_register_mod(arg % NUM_MODS);
        mod_count[arg % NUM_MODS]++;
        break;
    case 4:
        if (mod_count[arg % NUM_MODS] > 0) {
            zmk_hid_unregister_mod(arg % NUM_MODS);
            mod_count[arg % NUM_MODS]--;
        }
        break;
    case 5: {
        uint8_t flags = arg & 0xFF;
        zmk_hid_register_mods(flags);
        for (int i = 0; i < NUM_MODS; i++) {
            if (flags & (1 << i)) {
                mod_count[i]++;
            }
        }
        break;
    }
    case 6: {
        uint8_t flags = arg & 0xFF, ok = 0;
        for (int i = 0; i < NUM_MODS; i++) {
            if ((flags & (1 << i)) && mod_count[i] > 0) {
                ok |= 1 << i;
            }
        }
        zmk_hid_unregister_mods(ok);
        for (int i = 0; i < NUM_MODS; i++) {
            if (ok & (1 << i)) {
                mod_count[i]--;
            }
        }
        break;
    }
    case 7:
        implicit_mods = arg & 0xFF;
        zmk_hid_implicit_modifiers_press(implicit_mods);
        break;
    case 8:
        implicit_mods = 0;
        zmk_hid_implicit_modifiers_release();
        break;
    case 9:
        masked_mods = arg & 0xFF;
        zmk_hid_masked_modifiers_set(masked_mods);
        break;
    default:
        masked_mods = 0;
        zmk_hid_masked_modifiers_clear();
        break;
    }
}

static void unwind(void) {
    for (unsigned code = 1; code < KB_CODES; code++) {
        if (kb_state[code] != ABSENT) {
            keyboard_toggle(code, 0);
        }
    }
    for (unsigned code = 1; code < CONSUMER_CODES; code++) {
        if (consumer_state[code] != ABSENT) {
            consumer_toggle(code, 0);
        }
    }
    for (int i = 0; i < NUM_MODS; i++) {
        while (mod_count[i] > 0) {
            zmk_hid_unregister_mod(i);
            mod_count[i]--;
        }
    }
    implicit_mods = masked_mods = 0;
    zmk_hid_implicit_modifiers_release();
    zmk_hid_masked_modifiers_clear();
    check_all();
    if (kb_held || kb_dropped || consumer_held || consumer_dropped) {
        fail("state not empty after unwinding");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    for (size_t i = 0; i + 3 <= size; i += 3) {
        apply(data[i], data[i + 1] | (data[i + 2] << 8));
        check_all();
    }
    unwind();
    return 0;
}
