// TegraAPBDMA's interface stays what its users were written against (the UART channels, I2C3), and
// the word transfers (UART-D and I2C3 once, the rails' ring) are programmed as Linux's tegra20-apb-dma.c programs
// DMA_DEV_TO_MEM with a 4-byte slave width and maxburst 1 (no register model here: the source is checked).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "../TegraAPBDMA.h"
#include "bazel/testing/source_text.h"

// The fixed channels and request lines (tegra210.dtsi dmas = <&apbdma N>).
static_assert(TEGRA_APBDMA_CH_UARTB_RX == 0);
static_assert(TEGRA_APBDMA_CH_UARTC_RX == 1);
static_assert(TEGRA_APBDMA_CH_UARTD_TX == 2);
static_assert(TEGRA_APBDMA_CH_UARTD_RX == 3);
static_assert(TEGRA_APBDMA_CH_I2C3_RX == 4);
static_assert(TEGRA_APBDMA_REQ_UARTB == 9);
static_assert(TEGRA_APBDMA_REQ_UARTC == 10);
static_assert(TEGRA_APBDMA_REQ_UARTD == 19);
static_assert(TEGRA_APBDMA_REQ_I2C3 == 23);  // tegra210.dtsi i2c@7000c500 dmas

// The calls the UART drivers use, with the signatures they were written against.
static_assert(std::is_same_v<decltype(&tegra_apbdma_start_rx_words_ring),
                             void (*)(tegra_apbdma_channel *, uint32_t, unsigned, uint32_t, uint32_t)>);
static_assert(std::is_same_v<decltype(&tegra_apbdma_start_once),
                             void (*)(tegra_apbdma_channel *, uint32_t, unsigned, uint32_t, uint32_t)>);
static_assert(std::is_same_v<decltype(&tegra_apbdma_alloc), bool (*)(uint32_t, tegra_apbdma_buffer *)>);

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraAPBDMA, RxWordsProgrammedAsLinux)
{
    const std::string source = SourceText("../TegraAPBDMA.cpp");
    const std::string rx = Body(source, "void startRxWords(");
    // APB 32-bit with the address fixed, AHB 32-bit single-word bursts, flow-controlled on the request line, no DIR bit
    // (device to memory).
    EXPECT_THAT(rx, HasSubstr("APBSEQ_BUS_WIDTH_32 | APBSEQ_WRAP_WORD_1"));
    EXPECT_THAT(rx, HasSubstr("AHBSEQ_INTR_ENB | AHBSEQ_BUS_WIDTH_32 | AHBSEQ_BURST_1"));
    EXPECT_THAT(rx, HasSubstr("CSR_IE_EOC | once | CSR_FLOW"));
    EXPECT_THAT(rx, Not(HasSubstr("CSR_DIR")));
    EXPECT_THAT(source, HasSubstr("APBSEQ_BUS_WIDTH_32 = 2u << 28"));
    // A transfer once; the ring the same, continuous.
    EXPECT_THAT(Body(source, "void tegra_apbdma_start_rx_words("), HasSubstr("CSR_ONCE)"));
    EXPECT_THAT(Body(source, "void tegra_apbdma_start_rx_words_ring("), HasSubstr("size, 0)"));
}
