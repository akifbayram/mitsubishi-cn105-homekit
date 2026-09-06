/* Host tests for the retired automatic room-source id and the external-source
 * name-derived id, pinned against the shared header (main/sl2_proto.h, kept
 * byte-identical across every vendored copy — see room-source-no-auto design
 * doc). A drift here would mean this controller's copy of the header no
 * longer agrees with the ESPHome adapter's on what an id means. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "sl2_proto.h"

static void test_link_auto_id_is_not_mac_derived(void) {
    /* SL2_ROOM_SOURCE_LINK_AUTO_ID is namespace 3 with an all-zero MAC field
     * — sl2_room_source_id_mac() must not mistake it for a NS_LINK pin (that
     * would resurrect the retired automatic mode as a spurious all-zero
     * dial MAC instead of falling through to BAD_SOURCE/Internal). */
    uint8_t mac[6];
    assert(!sl2_room_source_id_mac(SL2_ROOM_SOURCE_LINK_AUTO_ID,
                                   SL2_ROOM_SOURCE_NS_LINK, mac));
}

static void test_external_name_ids_are_pinned(void) {
    /* Codegen and every controller must compute the same FNV-1a id for the
     * same name — pin the actual values so a change to the hash function (or
     * to the namespace byte placement) is caught here, not as a silent
     * catalog mismatch between the dial and the controller. */
    assert(sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant") ==
           UINT64_C(0x043d164670e68fd6));
    assert(sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Hallway") ==
           UINT64_C(0x04dddd7232305c79));
}

int main(void) {
    test_link_auto_id_is_not_mac_derived();
    test_external_name_ids_are_pinned();
    printf("sl2_room_source: all tests passed\n");
    return 0;
}
