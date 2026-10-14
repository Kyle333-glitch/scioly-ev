#pragma once

// Host-side mock of the Renesas reset-status register block (`bsp_api.h`
// -> `R7FA4M1AB.h`) sufficient to compile `src/bench/ResetCause.h` for host
// tests. All flags read zero unless the test sets them.

#include <cstdint>

struct MockRSystem {
    volatile uint32_t RSTSR0 = 0U;
    volatile uint32_t RSTSR1 = 0U;
};

extern MockRSystem mockRSystem;
#define R_SYSTEM (&mockRSystem)

#define R_SYSTEM_RSTSR0_PORF_Msk (1U << 0)
#define R_SYSTEM_RSTSR0_DPSRSTF_Msk (1U << 7)
#define R_SYSTEM_RSTSR1_WDTRF_Msk (1U << 1)
#define R_SYSTEM_RSTSR1_IWDTRF_Msk (1U << 2)
#define R_SYSTEM_RSTSR1_SWRF_Msk (1U << 3)
#define R_SYSTEM_RSTSR1_RPERF_Msk (1U << 4)
#define R_SYSTEM_RSTSR1_REERF_Msk (1U << 5)
#define R_SYSTEM_RSTSR1_BUSMRF_Msk (1U << 6)
#define R_SYSTEM_RSTSR1_BUSSRF_Msk (1U << 7)
#define R_SYSTEM_RSTSR1_SPERF_Msk (1U << 8)
