/* Known-answer tests for ZMK's HID report builder (app/src/hid.c), built with normal flags. */
#include <stdio.h>
#include <string.h>

#include <zmk/hid.h>

static int passed, failed;

#define CHECK(cond, name)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            passed++;                                                                              \
            printf("ok - %s\n", name);                                                             \
        } else {                                                                                   \
            failed++;                                                                              \
            printf("not ok - %s (line %d)\n", name, __LINE__);                                     \
        }                                                                                          \
    } while (0)

#define KEY(id) ZMK_HID_USAGE(HID_USAGE_KEY, id)
#define CONSUMER(id) ZMK_HID_USAGE(HID_USAGE_CONSUMER, id)

#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)
#define VARIANT "nkro"
#else
#define VARIANT "hkro"
#endif

static const uint8_t *kb_keys(void) { return zmk_hid_get_keyboard_report()->body.keys; }
static uint8_t kb_mods(void) { return zmk_hid_get_keyboard_report()->body.modifiers; }

static int kb_all_zero(void) {
    const uint8_t *k = kb_keys();
    for (size_t i = 0; i < sizeof(zmk_hid_get_keyboard_report()->body.keys); i++) {
        if (k[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    CHECK(zmk_hid_get_keyboard_report()->report_id == ZMK_HID_REPORT_ID_KEYBOARD,
          "keyboard report id");
    CHECK(zmk_hid_get_consumer_report()->report_id == ZMK_HID_REPORT_ID_CONSUMER,
          "consumer report id");
    CHECK(kb_all_zero() && kb_mods() == 0, "keyboard report starts empty");

    /* A */
    CHECK(zmk_hid_press(KEY(HID_USAGE_KEY_KEYBOARD_A)) == 0, "press A succeeds");
#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)
    CHECK(kb_keys()[0] == 0x10, "nkro: A sets bit 4 of byte 0");
#else
    CHECK(kb_keys()[0] == 0x04, "hkro: A is stored in slot 0");
#endif
    CHECK(zmk_hid_is_pressed(KEY(HID_USAGE_KEY_KEYBOARD_A)), "A is pressed");
    CHECK(!zmk_hid_is_pressed(KEY(HID_USAGE_KEY_KEYBOARD_B)), "B is not pressed");

    /* Left shift is a modifier, not a key */
    CHECK(zmk_hid_keyboard_press(HID_USAGE_KEY_KEYBOARD_LEFTSHIFT) == 1,
          "press left shift reports a modifier change");
    CHECK(kb_mods() == 0x02, "left shift sets modifier bit 1");
    CHECK(zmk_hid_get_explicit_mods() == 0x02, "left shift is an explicit modifier");
    CHECK(zmk_hid_is_pressed(KEY(HID_USAGE_KEY_KEYBOARD_LEFTSHIFT)), "left shift is pressed");

    /* A second key */
    CHECK(zmk_hid_press(KEY(HID_USAGE_KEY_KEYBOARD_Z)) == 0, "press Z succeeds");
#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)
    CHECK(kb_keys()[3] == 0x20, "nkro: Z (0x1D) sets bit 5 of byte 3");
#else
    CHECK(kb_keys()[1] == 0x1D, "hkro: Z is stored in slot 1");
#endif

    /* USB boot report */
    {
        zmk_hid_boot_report_t *b = zmk_hid_get_boot_report();
        CHECK(b->modifiers == 0x02, "boot report modifiers");
        CHECK(b->keys[0] == 0x04 && b->keys[1] == 0x1D, "boot report lists A and Z");
        CHECK(b->keys[2] == 0 && b->keys[5] == 0, "boot report pads with zero");
    }

    CHECK(zmk_hid_release(KEY(HID_USAGE_KEY_KEYBOARD_A)) == 0, "release A succeeds");
    CHECK(!zmk_hid_is_pressed(KEY(HID_USAGE_KEY_KEYBOARD_A)), "A is released");
    CHECK(zmk_hid_is_pressed(KEY(HID_USAGE_KEY_KEYBOARD_Z)), "Z is still pressed");
    zmk_hid_release(KEY(HID_USAGE_KEY_KEYBOARD_Z));
    CHECK(zmk_hid_keyboard_release(HID_USAGE_KEY_KEYBOARD_LEFTSHIFT) == 1,
          "release left shift reports a modifier change");
    CHECK(kb_mods() == 0 && kb_all_zero(), "keyboard report is empty again");

    /* Seven keys overflow the six-key boot report */
    for (int i = 0; i < 7; i++) {
        zmk_hid_press(KEY(HID_USAGE_KEY_KEYBOARD_A + i));
    }
    {
        zmk_hid_boot_report_t *b = zmk_hid_get_boot_report();
        int all_rollover = 1;
        for (int i = 0; i < 6; i++) {
            all_rollover &= b->keys[i] == HID_ERROR_ROLLOVER;
        }
        CHECK(all_rollover, "seven keys give a rollover boot report");
    }
    zmk_hid_release(KEY(HID_USAGE_KEY_KEYBOARD_A + 6));
    {
        zmk_hid_boot_report_t *b = zmk_hid_get_boot_report();
        CHECK(b->keys[0] == 0x04 && b->keys[5] == 0x09, "six keys give a normal boot report");
    }
    for (int i = 0; i < 6; i++) {
        zmk_hid_release(KEY(HID_USAGE_KEY_KEYBOARD_A + i));
    }
    CHECK(kb_all_zero(), "all keys released");

#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_HKRO)
    /* HKRO reuses the lowest free slot */
    zmk_hid_press(KEY(0x04));
    zmk_hid_press(KEY(0x05));
    zmk_hid_press(KEY(0x06));
    zmk_hid_release(KEY(0x05));
    CHECK(kb_keys()[0] == 0x04 && kb_keys()[1] == 0 && kb_keys()[2] == 0x06,
          "hkro: release frees the middle slot");
    zmk_hid_press(KEY(0x07));
    CHECK(kb_keys()[1] == 0x07, "hkro: next press takes the lowest free slot");
    {
        zmk_hid_boot_report_t *b = zmk_hid_get_boot_report();
        CHECK(b->keys[0] == 0x04 && b->keys[1] == 0x07 && b->keys[2] == 0x06 && b->keys[3] == 0,
              "hkro: boot report is compacted in slot order");
    }
    zmk_hid_release(KEY(0x04));
    zmk_hid_release(KEY(0x06));
    zmk_hid_release(KEY(0x07));
    for (int i = 0; i < CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE + 1; i++) {
        zmk_hid_press(KEY(0x10 + i));
    }
    CHECK(zmk_hid_is_pressed(KEY(0x10 + CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE - 1)),
          "hkro: last slot is used");
    CHECK(!zmk_hid_is_pressed(KEY(0x10 + CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE)),
          "hkro: key beyond the report size is dropped");
    for (int i = 0; i < CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE + 1; i++) {
        zmk_hid_release(KEY(0x10 + i));
    }
    CHECK(kb_all_zero(), "hkro: all slots free again");
#else
    /* NKRO ignores usages beyond its bitmap */
    CHECK(zmk_hid_press(KEY(0x80)) == 0 && kb_all_zero() && !zmk_hid_is_pressed(KEY(0x80)),
          "nkro: usage beyond the bitmap is ignored");
    zmk_hid_release(KEY(0x80));
    CHECK(zmk_hid_get_boot_report()->keys[0] == 0, "nkro: ignored usage leaves boot report empty");
#endif

    /* Consumer page */
    CHECK(zmk_hid_press(CONSUMER(0xE9)) == 0, "press volume up succeeds");
    CHECK(zmk_hid_get_consumer_report()->body.keys[0] == 0xE9, "consumer report holds 0xE9");
    CHECK(zmk_hid_is_pressed(CONSUMER(0xE9)), "volume up is pressed");
    CHECK(zmk_hid_consumer_press(0xFFF) == 0, "highest full consumer usage is accepted");
    CHECK(zmk_hid_get_consumer_report()->body.keys[1] == 0xFFF, "consumer slot 1 holds 0xFFF");
    CHECK(zmk_hid_consumer_press(0x1000) == -ENOTSUP, "consumer usage above 0xFFF is rejected");
    zmk_hid_release(CONSUMER(0xFFF));
    CHECK(zmk_hid_release(CONSUMER(0xE9)) == 0, "release volume up succeeds");
    CHECK(zmk_hid_get_consumer_report()->body.keys[0] == 0 &&
              zmk_hid_get_consumer_report()->body.keys[1] == 0,
          "consumer report is empty again");

    /* Modifier bookkeeping */
    CHECK(zmk_hid_register_mod(0) == 1 && kb_mods() == 0x01, "register left control");
    CHECK(zmk_hid_register_mod(0) == 0, "second registration does not change the report");
    CHECK(zmk_hid_unregister_mod(0) == 0 && kb_mods() == 0x01,
          "modifier stays while a registration remains");
    CHECK(zmk_hid_implicit_modifiers_press(0x04) == 1 && kb_mods() == 0x05,
          "implicit left alt is added");
    CHECK(zmk_hid_masked_modifiers_set(0x01) == 1 && kb_mods() == 0x04,
          "masked left control is hidden");
    CHECK(zmk_hid_get_explicit_mods() == 0x01, "masking does not change explicit modifiers");
    zmk_hid_masked_modifiers_clear();
    CHECK(kb_mods() == 0x05, "clearing the mask restores left control");
    zmk_hid_implicit_modifiers_release();
    CHECK(zmk_hid_unregister_mod(0) == 1 && kb_mods() == 0, "last unregistration clears the bit");
    CHECK(zmk_hid_unregister_mod(5) == -EINVAL, "unregistering an unheld modifier fails");
    CHECK(zmk_hid_register_mods(0xC0) == 2 && kb_mods() == 0xC0, "register two modifiers at once");
    CHECK(zmk_hid_unregister_mods(0xC0) == 2 && kb_mods() == 0, "unregister both again");

    printf("KAT hid-%s passed=%d failed=%d\n", VARIANT, passed, failed);
    return failed ? 1 : 0;
}
