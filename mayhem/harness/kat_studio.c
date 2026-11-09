/* Known-answer tests for ZMK Studio's frame layer and Request messages, built with normal flags. */
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <proto/zmk/studio.pb.h>

#include "studio/msg_framing.h"

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

static bool step(enum studio_framing_state *s, uint8_t b, enum studio_framing_state want_state,
                 bool want_data) {
    bool data = studio_framing_process_byte(s, b);
    return data == want_data && *s == want_state;
}

int main(void) {
    enum studio_framing_state s = FRAMING_STATE_IDLE;

    CHECK(step(&s, 0x01, FRAMING_STATE_IDLE, false), "data before SOF is discarded");
    CHECK(step(&s, FRAMING_SOF, FRAMING_STATE_AWAITING_DATA, false), "SOF starts a frame");
    CHECK(step(&s, 0x10, FRAMING_STATE_AWAITING_DATA, true), "plain byte is data");
    CHECK(step(&s, FRAMING_ESC, FRAMING_STATE_ESCAPED, false), "ESC escapes the next byte");
    CHECK(step(&s, FRAMING_SOF, FRAMING_STATE_AWAITING_DATA, true), "escaped SOF is data");
    CHECK(step(&s, FRAMING_ESC, FRAMING_STATE_ESCAPED, false), "second ESC");
    CHECK(step(&s, FRAMING_ESC, FRAMING_STATE_AWAITING_DATA, true), "escaped ESC is data");
    CHECK(step(&s, FRAMING_EOF, FRAMING_STATE_EOF, false), "EOF ends the frame");
    CHECK(step(&s, FRAMING_SOF, FRAMING_STATE_AWAITING_DATA, false), "SOF restarts after EOF");
    CHECK(step(&s, FRAMING_SOF, FRAMING_STATE_ERR, false), "unescaped SOF mid-frame is an error");
    CHECK(step(&s, 0x22, FRAMING_STATE_ERR, false), "data is discarded in the error state");
    CHECK(step(&s, FRAMING_EOF, FRAMING_STATE_IDLE, false), "EOF leaves the error state");

    {
        const uint8_t req[] = {0x08, 0x01, 0x1a, 0x02, 0x08, 0x01};
        zmk_studio_Request r = zmk_studio_Request_init_zero;
        pb_istream_t is = pb_istream_from_buffer(req, sizeof(req));
        CHECK(pb_decode(&is, &zmk_studio_Request_msg, &r), "decode get_device_info request");
        CHECK(r.request_id == 1, "request id is 1");
        CHECK(r.which_subsystem == zmk_studio_Request_core_tag, "subsystem is core");
        CHECK(r.subsystem.core.which_request_type == zmk_core_Request_get_device_info_tag &&
                  r.subsystem.core.request_type.get_device_info,
              "request is get_device_info");
    }

    {
        const uint8_t req[] = {0x08, 0x03, 0x2a, 0x19, 0x62, 0x17, 0x08, 0x01, 0x12, 0x13,
                               'a',  'b',  'c',  'd',  'e',  'f',  'g',  'h',  'i',  'j',
                               'k',  'l',  'm',  'n',  'o',  'p',  'q',  'r',  's'};
        zmk_studio_Request r = zmk_studio_Request_init_zero;
        pb_istream_t is = pb_istream_from_buffer(req, sizeof(req));
        CHECK(pb_decode(&is, &zmk_studio_Request_msg, &r), "decode set_layer_props request");
        CHECK(r.which_subsystem == zmk_studio_Request_keymap_tag, "subsystem is keymap");
        CHECK(r.subsystem.keymap.which_request_type == zmk_keymap_Request_set_layer_props_tag,
              "request is set_layer_props");
        CHECK(r.subsystem.keymap.request_type.set_layer_props.layer_id == 1, "layer id is 1");
        CHECK(memcmp(r.subsystem.keymap.request_type.set_layer_props.name,
                     "abcdefghijklmnopqrs\0", 20) == 0,
              "layer name round-trips");
    }

    {
        const uint8_t bad[] = {0x08, 0x01, 0x1a, 0x05, 0x08};
        zmk_studio_Request r = zmk_studio_Request_init_zero;
        pb_istream_t is = pb_istream_from_buffer(bad, sizeof(bad));
        CHECK(!pb_decode(&is, &zmk_studio_Request_msg, &r), "truncated request is rejected");
    }

    {
        zmk_studio_Request r = zmk_studio_Request_init_zero;
        r.request_id = 7;
        r.which_subsystem = zmk_studio_Request_core_tag;
        r.subsystem.core.which_request_type = zmk_core_Request_get_device_info_tag;
        r.subsystem.core.request_type.get_device_info = true;
        uint8_t out[16];
        pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
        const uint8_t want[] = {0x08, 0x07, 0x1a, 0x02, 0x08, 0x01};
        CHECK(pb_encode(&os, &zmk_studio_Request_msg, &r) && os.bytes_written == sizeof(want) &&
                  memcmp(out, want, sizeof(want)) == 0,
              "encode request bytes");
    }

    printf("KAT studio passed=%d failed=%d\n", passed, failed);
    return failed ? 1 : 0;
}
