
#ifndef __GARAGE_PWM_H__
#define __GARAGE_PWM_H__

#define PWM_BASE        (BCM2708_PERI_BASE + 0x20C000)

#define PWM_CTRL 0x00
#define PWM_STAT 0x04
#define PWM_DMAC 0x08
#define PWM_FIFO 0x18
#define PWM_RNG1 0x10
#define PWM_RNG2 0x20
#define PWM_DAT1 0x14
#define PWM_DAT2 0x24

#define PWMCTRL_PWEN1   BIT(0)
#define PWMCTRL_MODE1   BIT(1)
#define PWMCTRL_RPTL1   BIT(2)
#define PWMCTRL_CLRF    BIT(6)
#define PWMCTRL_PWEN2   BIT(8)
#define PWMCTRL_RPTL2   BIT(10)
#define PWMCTRL_USEF2   BIT(13)
#define PWMCTRL_MSEN2   BIT(15)

#define PWMDMAC_ENAB    BIT(31)

struct garage_dev;

void pwm_stop(struct garage_dev *g);

void pwm_init(struct garage_dev *g, int dma);

#endif
