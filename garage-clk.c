
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/timekeeping.h>

#include "garage-driver.h"
#include "garage-clk.h"

int pwm_clock_init(struct garage_dev *g, int freq)
{
    int divi, divf;
    long long tmp;
    int mash;
    u32 ctl;

    if(g->freq < 1000000L || g->freq > 500000000L) {
        return -EINVAL;
    }

    if(g->freq < 100000000L)
        mash = 3;
    else if(g->freq <= 150000000L)
        mash = 2;
    else
        mash = 1;

    ctl = CLK_PASSWD | CLKCNTL_MASH(mash) | PLL_1GHZ;
    divi = GHZ/freq;
    tmp = 0x1000LL*(GHZ%freq);
    do_div(tmp, freq);
    divf = (int) tmp;

    writel(ctl, g->clk_reg + PWMCLK_CNTL); // disable clock
    writel(CLK_PASSWD | CLKDIV_DIVI(divi) | CLKDIV_DIVF(divf), g->clk_reg + PWMCLK_DIV); // set div ratio
    writel(ctl | CLKCNTL_ENAB, g->clk_reg + PWMCLK_CNTL); // enable clock

    return 0;
}


void pwm_clock_stop(struct garage_dev *g)
{
    writel(CLK_PASSWD | CLKCNTL_MASH(0) | PLL_500MHZ, g->clk_reg + PWMCLK_CNTL);
}
