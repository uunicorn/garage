
#ifndef __GARAGE_CLK_H__
#define __GARAGE_CLK_H__

#define CLK_BASE        (BCM2708_PERI_BASE + 0x101000)
#define PWMCLK_CNTL     0xa0
#define PWMCLK_DIV      0xa4

#define GHZ             1000000000

#define PLL_192MHZ      0x1
#define PLL_1GHZ        0x5
#define PLL_500MHZ      0x6

#define CLK_PASSWD      0x5A000000

#define CLKCNTL_ENAB    (1 << 4)
#define CLKCNTL_MASH(x) (x << 9)

#define CLKDIV_DIVI(x)  (x << 12)
#define CLKDIV_DIVF(x)  (x << 0)

struct garage_dev;

void pwm_clock_init(struct garage_dev *g, int freq);
void pwm_clock_stop(struct garage_dev *g);

#endif
