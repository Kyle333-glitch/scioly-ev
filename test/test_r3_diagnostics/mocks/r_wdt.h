#pragma once

// Host-side mock of the Renesas WDT register block (`r_wdt.h`) sufficient to
// compile `src/bench/WatchdogRegisters.h` for host tests. Registers read
// zero and the peripheral clock reads a fixed value unless the test sets
// them.

#include <cstdint>

struct MockRWdt {
    volatile uint32_t WDTCR = 0U;
    volatile uint32_t WDTSR = 0U;
};

extern MockRWdt mockRWdt;
#define R_WDT (&mockRWdt)

#define R_WDT_WDTCR_TOPS_Msk (0x3U << 0)
#define R_WDT_WDTCR_TOPS_Pos (0U)
#define R_WDT_WDTCR_CKS_Msk (0xFU << 4)
#define R_WDT_WDTCR_CKS_Pos (4U)
#define R_WDT_WDTCR_RPES_Msk (0x3U << 8)
#define R_WDT_WDTCR_RPES_Pos (8U)
#define R_WDT_WDTCR_RPSS_Msk (0x3U << 12)
#define R_WDT_WDTCR_RPSS_Pos (12U)
#define R_WDT_WDTSR_CNTVAL_Msk (0x3FFFU << 0)
#define R_WDT_WDTSR_CNTVAL_Pos (0U)
#define R_WDT_WDTSR_UNDFF_Msk (1U << 14)
#define R_WDT_WDTSR_REFEF_Msk (1U << 15)

#define FSP_PRIV_CLOCK_PCLKB (2)

inline uint32_t R_FSP_SystemClockHzGet(uint32_t /*clock*/) { return 24000000U; }
