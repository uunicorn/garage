
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>

#include "garage-driver.h"
#include "garage-gpio.h"
#include "garage-pwm.h"
#include "garage-dma.h"

#define DRVNAME "garage-door"

#define BUSY_LED_PIN 19

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


// AM sequence
const char * const code = "111110110110010010010010010010110110010010010110111111101100100100100100100101101100100100101101111111011001001001001001001011011001001001011011111110110010010010010010010110110010010010110111111101100100100100100100101101100100100101101111110";

static struct platform_device *pdev;

static int garage_allocate_resources(struct garage_dev *g)
{
    int err;

    g->gpio_reg = ioremap(GPIO_BASE, SZ_16K);
    g->pwm_reg = ioremap(PWM_BASE, SZ_16K);
    g->clk_reg = ioremap(CLK_BASE, SZ_16K);

    if((err = dma_allocate(g)) < 0)
        return err;

    return 0;
}

static void garage_release_resources(struct garage_dev *g)
{
    dma_release(g);
    iounmap(g->gpio_reg);
    iounmap(g->pwm_reg);
    iounmap(g->clk_reg);
}

static void garage_stop(struct garage_dev *g)
{
    if(g->gpio_reg) {
        gpio_set_mode(g, 18, 1);
    }

    if(g->clk_reg) {
        // stop all the clocks...
        writel(CLK_PASSWD | CLKCNTL_MASH(0) | PLL_500MHZ, g->clk_reg + PWMCLK_CNTL);
    }

    if(g->pwm_reg) {
        pwm_stop(g);
    }

    if(g->dma_chan_base) {
        dma_reset(g);
    }
}

void garage_dma_done(void *data)
{
    struct garage_dev *g = data;
    ktime_t diff = ktime_sub(ktime_get(), g->start_time);

    garage_stop(g);

    printk(KERN_INFO "all done: %ld ms\n", (long)ktime_to_ms(diff));
}

static void init_pwm_clock(struct garage_dev *g, int freq)
{
    int divi, divf;
    long long tmp;
    u32 ctl = CLK_PASSWD | CLKCNTL_MASH(3) | PLL_1GHZ;

    divi = GHZ/freq;
    tmp = 0x1000LL*(GHZ%freq);
    do_div(tmp, freq);
    divf = (int) tmp;

    writel(ctl, g->clk_reg + PWMCLK_CNTL); // disable clock
    writel(CLK_PASSWD | CLKDIV_DIVI(divi) | CLKDIV_DIVF(divf), g->clk_reg + PWMCLK_DIV); // set div ratio
    writel(ctl | CLKCNTL_ENAB, g->clk_reg + PWMCLK_CNTL); // enable clock
}

static void outbit(struct garage_dev *g, int bit)
{
     // set PWM1 pattern (amplitude)
    add_xfer(g, g->buf_handle+4+(bit ? 4 : 0), PHYS_TO_DMA(PWM_BASE + PWM_DAT1), 4);

    // wait 1 full sample rate period
    add_xfer(g, g->buf_handle+4*3, PHYS_TO_DMA(PWM_BASE + PWM_FIFO), 4)
        ->info |= BCM2708_DMA_PER_MAP(5) | BCM2708_DMA_D_DREQ; 
}

static int garage_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct garage_dev *g = kzalloc(sizeof(struct garage_dev), GFP_KERNEL);
    u32 *buf;
    int err, width;
    const char *p;

    platform_set_drvdata(pdev, g);
    g->dev = dev;
    g->dma_chan_base = NULL;
    g->freq = 40685000; // 40MHz carrier
    g->srate = 1250; // 1250Hz = 800us period

    width = 2*g->freq/g->srate;

    if((err = garage_allocate_resources(g)) < 0) {
        garage_release_resources(g);
        return err;
    }

    gpio_set_mode(g, 18, 2);                // pin18 -> PWM out
    gpio_set_mode(g, BUSY_LED_PIN, 1);      // GPIO out (busy led)
    gpio_set(g, BUSY_LED_PIN);              // busy led ON

    buf = (u32*)(g->cb_base + MAX_CBS);
    g->buf_handle = g->cb_handle + sizeof(*g->cb_base)*MAX_CBS;

    // set PWM clock to 2x carrier frequency 
    // (2x, because 101010...1010b serializer pattern divides clock frequency by two)
    init_pwm_clock(g, g->freq*2);

    pwm_stop(g);

    pwm_init(g, 0); // start PWM, but keep DREQ low

    if((err = start_dummy_tx(g)) < 0) {
        garage_release_resources(g);
        return err;
    }

    // setup the buffer
    buf[0] = GPIO_BIT(BUSY_LED_PIN);       // busy led pin
    buf[1] = 0;             // amplitude == 0
    buf[2] = 0xaaaaaaaa;    // amplitude == max (1010101010...1010b)
    buf[3] = width/2;       // don't care, but 50% duty cycle could be useful for debugging

    g->sample = 0;

    for(p=code;*p;p++) {
        outbit(g, *p == '1');
    }

    add_xfer(g, g->buf_handle, PHYS_TO_DMA(GPIO_BASE + GPIO_REG_CLEAR(BUSY_LED_PIN)), 4)
        ->info |= BCM2708_DMA_INT_EN;

    dma_reset(g);

    bcm_dma_start(g->dma_chan_base, g->cb_handle);
    g->start_time = ktime_get();

    pwm_init(g, 1); // restart PWM, enable DMA

    return 0;
}

static int garage_remove(struct platform_device *pdev)
{
    struct garage_dev *g = platform_get_drvdata(pdev);

    gpio_clear(g, BUSY_LED_PIN);

    garage_stop(g);

    garage_release_resources(g);

    printk(KERN_INFO "Goodbye world.\n");

    return 0;
}

static struct platform_driver garage_driver = {
	.probe		= garage_probe,
	.remove		= garage_remove,
	.driver		= {
		.name		= DRVNAME,
	},
};

static int __init garage_init(void)
{
    int ret;

    ret = platform_driver_register(&garage_driver);
    if(ret < 0)
        return ret;

    pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);
    if (IS_ERR(pdev)) {
        platform_driver_unregister(&garage_driver);
        return PTR_ERR(pdev);
    }


    return 0;
}

module_init(garage_init);

static void __exit garage_exit(void)
{
    platform_device_unregister(pdev);
    platform_driver_unregister(&garage_driver);
}

module_exit(garage_exit);

MODULE_LICENSE("GPL");

