#ifndef PSXRECOMP_V4_INTERRUPTS_H
#define PSXRECOMP_V4_INTERRUPTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* IRQ bit positions in I_STAT/I_MASK */
#define IRQ_VBLANK  0
#define IRQ_GPU     1
#define IRQ_CDROM   2
#define IRQ_DMA     3
#define IRQ_TIMER0  4
#define IRQ_TIMER1  5
#define IRQ_TIMER2  6
#define IRQ_SIO0    7
#define IRQ_SIO1    8
#define IRQ_SPU     9
#define IRQ_PIO     10

void interrupts_init(void);

/* Called from the dispatch loop after each function returns.
 * Fires vblank on schedule, checks (i_stat & i_mask), and if
 * pending + COP0 allows, dispatches the exception handler. */
void psx_check_interrupts(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_V4_INTERRUPTS_H */
