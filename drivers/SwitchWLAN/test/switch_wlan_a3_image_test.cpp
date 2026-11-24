// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <algorithm>
#include <array>
#include <cassert>
#include <vector>
#include "drivers/SwitchWLAN/firmware/a3/image.h"

static void switch_candidate_image()
{
    assert(switch_a3_state_range_valid(0x210b38, 0x180000, 0xc0000));
    assert(switch_a3_state_range_valid(0x240000 - 64, 0x180000, 0xc0000));
    assert(!switch_a3_state_range_valid(0x240000 - 60, 0x180000, 0xc0000));
    assert(!switch_a3_state_range_valid(0x210b39, 0x180000, 0xc0000));
    assert(!switch_a3_state_range_valid(0x17fffc, 0x180000, 0xc0000));
    assert(!switch_a3_state_range_valid(0x210b38, 0x180000, 60));
    assert(!switch_a3_state_range_valid(0xfffffff0, 0xfffffff0, 64));
    std::vector<uint8_t> image(SWITCH_A3_IMAGE_BYTES, 0x7a);
    const std::array<uint32_t, 10> sites = {SWITCH_A3_HEAP_CALL,   SWITCH_A3_ATTACH_RETURN,  SWITCH_A3_RX_CALL,
                                           SWITCH_A3_TX_POINTER,  SWITCH_A3_TX_STATUS_CALL, SWITCH_A3_PM_STATUS_CALL,
                                           SWITCH_A3_CHANNEL_CALL, SWITCH_A3_PM_PREPARE_BRANCH, SWITCH_A3_PM_SUBMIT_CALL, SWITCH_A3_PM_SET_ENTRY};
    const std::array<std::array<uint8_t, 4>, 10> original = {{{0xff, 0xf7, 0x96, 0xfd},
                                                             {0x20, 0x46, 0x11, 0xb0},
                                                             {0xf5, 0xf7, 0x74, 0xf9},
                                                             {0xa5, 0xc7, 0x18, 0x00},
                                                             {0xc6, 0xf6, 0x21, 0xfe},
                                                             {0xff, 0xf7, 0x98, 0xfa},
                                                             {0xc3, 0xf7, 0x2b, 0xfe},
                                                             {0x1b, 0x7a, 0x53, 0xb9},
                                                             {0xff, 0xf7, 0x2e, 0xff},
                                                             {0xd0, 0xf8, 0x20, 0x31}}};
    for (size_t index = 0; index < sites.size(); ++index)
        std::copy(original[index].begin(), original[index].end(), image.begin() + sites[index] - SWITCH_A3_IMAGE_BASE);
    std::vector<uint8_t> payload(SWITCH_A3_CANDIDATE_MAX_BYTES);
    std::vector<uint8_t> out(SWITCH_A3_PAYLOAD_BASE - SWITCH_A3_IMAGE_BASE + payload.size(), 0xa5);
    const switch_a3_candidate_entries entries = {0x20e7dc, 0x20e810, 0x20e840, 0x20e870, 0x20e8a0, 0x20e8d0, 0x20e900, 0x20e930, 0x20e960, 0x20e990};
    assert(switch_a3_patch_candidate_image(image.data(), image.size(), out.data(), out.size(), payload.data(),
                                           payload.size(), &entries));
    const auto *pointer = out.data() + SWITCH_A3_TX_POINTER - SWITCH_A3_IMAGE_BASE;
    assert(pointer[0] == 0x71 && pointer[1] == 0xe8 && pointer[2] == 0x20 && pointer[3] == 0);
    assert(std::equal(payload.begin(), payload.end(), out.end() - payload.size()));
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                  entries.heap, entries.attach, entries.rx));
    for (const auto size : {size_t(0), payload.size() - 1, payload.size() + 4})
        assert(!switch_a3_patch_candidate_image(image.data(), image.size(), out.data(), out.size(), payload.data(),
                                                size, &entries));
    const auto unchanged = out;
    for (const auto site : sites) {
        image[site - SWITCH_A3_IMAGE_BASE] ^= 1;
        assert(!switch_a3_patch_candidate_image(image.data(), image.size(), out.data(), out.size(), payload.data(),
                                                payload.size(), &entries));
        assert(out == unchanged);
        image[site - SWITCH_A3_IMAGE_BASE] ^= 1;
    }
    for (const auto entry :
         {&switch_a3_candidate_entries::heap, &switch_a3_candidate_entries::attach, &switch_a3_candidate_entries::rx,
          &switch_a3_candidate_entries::tx, &switch_a3_candidate_entries::tx_status,
          &switch_a3_candidate_entries::pm_status, &switch_a3_candidate_entries::channel,
          &switch_a3_candidate_entries::pm_prepare, &switch_a3_candidate_entries::pm_submit,
          &switch_a3_candidate_entries::pm_set})
        for (const auto value : {SWITCH_A3_PAYLOAD_BASE - 2u, SWITCH_A3_PAYLOAD_BASE + 1u,
                                 SWITCH_A3_PAYLOAD_BASE + static_cast<uint32_t>(payload.size())}) {
            auto invalid = entries;
            invalid.*entry = value;
            assert(!switch_a3_patch_candidate_image(image.data(), image.size(), out.data(), out.size(), payload.data(),
                                                    payload.size(), &invalid));
            assert(out == unchanged);
        }
}

int main()
{
    std::vector<uint8_t> image(SWITCH_A3_IMAGE_BYTES, 0x7a);
    const std::array<uint8_t, 4> heap = {0xff, 0xf7, 0x96, 0xfd};
    const std::array<uint8_t, 4> rx = {0xf5, 0xf7, 0x74, 0xf9};
    const std::array<uint8_t, 4> attach = {0x20, 0x46, 0x11, 0xb0};
    std::copy(heap.begin(), heap.end(), image.begin() + SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE);
    std::copy(attach.begin(), attach.end(), image.begin() + SWITCH_A3_ATTACH_RETURN - SWITCH_A3_IMAGE_BASE);
    std::copy(rx.begin(), rx.end(), image.begin() + SWITCH_A3_RX_CALL - SWITCH_A3_IMAGE_BASE);
    std::array<uint8_t, 404> payload{};
    std::vector<uint8_t> out(SWITCH_A3_PAYLOAD_BASE - SWITCH_A3_IMAGE_BASE + payload.size(), 0xa5);
    assert(switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                 0x20e7dc, 0x20e80c, 0x20e820));
    const std::array<uint8_t, 4> heap_branch = {0x2e, 0xf0, 0xb6, 0xfb};
    const std::array<uint8_t, 4> attach_branch = {0x24, 0xf0, 0x3d, 0xbe};
    assert(!memcmp(out.data() + SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE, heap_branch.data(), 4));
    assert(!memcmp(out.data() + SWITCH_A3_ATTACH_RETURN - SWITCH_A3_IMAGE_BASE, attach_branch.data(), 4));
    assert(out.front() == image.front());
    assert(out[image.size()] == 0 && out[image.size() + 2] == 0);
    assert(std::equal(payload.begin(), payload.end(), out.end() - payload.size()));
    const auto unchanged = out;
    image[SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE] ^= 1;
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                  0x20e7dc, 0x20e80c, 0x20e820));
    assert(out == unchanged);
    image[SWITCH_A3_HEAP_CALL - SWITCH_A3_IMAGE_BASE] ^= 1;
    for (const auto size : {size_t(0), payload.size() - 1, size_t(SWITCH_A3_MAX_PAYLOAD_BYTES + 4)})
        assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), size,
                                      0x20e7dc, 0x20e80c, 0x20e820));
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size() - 1, payload.data(),
                                  payload.size(), 0x20e7dc, 0x20e80c, 0x20e820));
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                  0x20e7dd, 0x20e80c, 0x20e820));
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                  0x20e7dc, SWITCH_A3_PAYLOAD_BASE + payload.size(), 0x20e820));
    assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(), payload.size(),
                                  0x20e7dc, 0x20e80c, 0x20e821));
    for (const auto site : {SWITCH_A3_RX_CALL, SWITCH_A3_ATTACH_RETURN}) {
        image[site - SWITCH_A3_IMAGE_BASE] ^= 1;
        assert(!switch_a3_patch_image(image.data(), image.size(), out.data(), out.size(), payload.data(),
                                      payload.size(), 0x20e7dc, 0x20e80c, 0x20e820));
        assert(out == unchanged);
        image[site - SWITCH_A3_IMAGE_BASE] ^= 1;
    }
    assert(out == unchanged);
    switch_candidate_image();
}
