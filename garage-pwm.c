
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/timekeeping.h>

#include "garage-driver.h"
#include "garage-pwm.h"

void pwm_stop(struct garage_dev *g)
{
    writel(PWMCTRL_CLRF, g->pwm_reg + PWM_CTRL); // stop both channels, clear fifo
    writel(0, g->pwm_reg + PWM_DMAC); // disable DMA
}

void pwm_init(struct garage_dev *g, int dma)
{
    int width = 2*g->freq/g->srate;

    writel(32, g->pwm_reg + PWM_RNG1); // set PWM1 pattern width to 32 bits
    writel(0, g->pwm_reg + PWM_DAT1); // set initial amplitude to zero (seializing zero)

    writel(width, g->pwm_reg + PWM_RNG2);

    // enable channels:
    // PWM1 - 32bit serializer mode, no FIFO, repeat
    // PWM2 - M/S mode, FIFO, no repeat
    writel(PWMCTRL_CLRF | 
            PWMCTRL_MODE1 | PWMCTRL_PWEN1 | PWMCTRL_RPTL1 | 
            PWMCTRL_MSEN2 | PWMCTRL_PWEN2 | PWMCTRL_USEF2, 
            g->pwm_reg + PWM_CTRL);

    if(dma) {
        writel(PWMDMAC_ENAB | 1, g->pwm_reg + PWM_DMAC); // enable DMA, 1 word threshold
    }
}
