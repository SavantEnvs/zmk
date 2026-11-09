/*
 * Fuzz harness for the ZMK Studio RPC receive path.
 *
 * rpc_read_cb() in app/src/studio/rpc.c feeds bytes from the transport ring buffer through
 * studio_framing_process_byte() (the real app/src/studio/msg_framing.c) and hands the unescaped data
 * to nanopb's pb_decode() for zmk_studio_Request (the real nanopb with the real generated messages).
 * This harness runs the same pipeline over the fuzz input, decoding successive frames from it the way
 * rpc_main() does, and checks that a request that decoded can be re-encoded and decoded again.
 * The framing state is local to each input.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <proto/zmk/studio.pb.h>

#include "studio/msg_framing.h"

struct source {
    const uint8_t *data;
    size_t size;
    size_t off;
    enum studio_framing_state state;
};

static bool read_cb(pb_istream_t *stream, uint8_t *buf, size_t count) {
    struct source *src = stream->state;
    size_t written = 0;

    while (written < count && src->state != FRAMING_STATE_EOF) {
        if (src->off >= src->size) {
            return false;
        }
        uint8_t b = src->data[src->off++];
        if (studio_framing_process_byte(&src->state, b)) {
            buf[written++] = b;
        }
    }

    if (src->state == FRAMING_STATE_EOF) {
        stream->bytes_left = 0;
        return false;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct source src = {.data = data, .size = size, .off = 0, .state = FRAMING_STATE_IDLE};

    while (src.off < src.size) {
        size_t before = src.off;
        pb_istream_t stream = {&read_cb, &src, SIZE_MAX};
        zmk_studio_Request req = zmk_studio_Request_init_zero;
        bool ok = pb_decode(&stream, &zmk_studio_Request_msg, &req);
        src.state = FRAMING_STATE_IDLE;

        if (ok) {
            uint8_t out[1024];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            if (pb_encode(&os, &zmk_studio_Request_msg, &req)) {
                zmk_studio_Request again = zmk_studio_Request_init_zero;
                pb_istream_t is = pb_istream_from_buffer(out, os.bytes_written);
                if (!pb_decode(&is, &zmk_studio_Request_msg, &again)) {
                    abort();
                }
            }
        }

        if (src.off == before) {
            break;
        }
    }
    return 0;
}
