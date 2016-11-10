
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
#include <linux/dmaengine.h>
#include <linux/platform_data/dma-bcm2708.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>

#define DRVNAME "garage-door"

#define BUSY_LED_PIN 19

#define PHYS_TO_DMA(x) (0x7E000000 - BCM2708_PERI_BASE + x)

#define PWM_BASE (BCM2708_PERI_BASE + 0x20C000)
#define CLK_BASE (BCM2708_PERI_BASE + 0x101000)
#define PWMCLK_CNTL 0x28
#define PWMCLK_DIV 0x29

#define GHZ         1000000000

#define PLL_192MHZ  0x1
#define PLL_1GHZ    0x5
#define PLL_500MHZ  0x6

#define CLK_PASSWD  0x5A000000

#define CLKCNTL_ENAB    (1 << 4)
#define CLKCNTL_MASH(x) (x << 9)

#define CLKDIV_DIVI(x) (x << 12)
#define CLKDIV_DIVF(x) (x << 0)

#define PWM_CTRL 0x00
#define PWM_STAT 0x04
#define PWM_DMAC 0x08
#define PWM_FIFO 0x18
#define PWM_RNG1 0x10
#define PWM_RNG2 0x20
#define PWM_DAT1 0x14
#define PWM_DAT2 0x24

#define PWEN1   BIT(0)
#define MODE1   BIT(1)
#define RPTL1   BIT(2)
#define CLRF    BIT(6)
#define PWEN2   BIT(8)
#define RPTL2   BIT(10)
#define USEF2   BIT(13)
#define MSEN2   BIT(15)

#define GPIO_REG_SET(x)     (x < 32 ? 0x1c : 0x20)
#define GPIO_REG_CLEAR(x)   (x < 32 ? 0x28 : 0x2c)
#define GPIO_BIT(x)         BIT(x < 32 ? x : (x - 32))

// reserve this number of DMA control blocks
#define MAX_CBS 600

// number of words to transfer per single sample. Must be at least the size of the PWM FIFO
#define XFR_SZ 16 

// AM sequence
const char * const code = "111110110110010010010010010010110110010010010110111111101100100100100100100101101100100100101101111111011001001001001001001011011001001001011011111110110010010010010010010110110010010010110111111101100100100100100100101101100100100101101111110";

struct garage_dev {
    struct device *dev;

    void *pwm_reg, *dma_reg, *dma_chan_base, *gpio_reg;
    u32 *clk_reg;

    struct dma_chan *dma_chan;
    struct bcm2708_dma_cb *cb_base;		/* DMA control blocks */
    dma_addr_t cb_handle, buf_handle;
    struct scatterlist sg;
    int sample;
    int freq;
    int srate;
    ktime_t start_time;
};

static struct platform_device *pdev;


static void gpio_set_mode(struct garage_dev *g, unsigned gpio, unsigned mode)
{
    int shift;
    void *reg;

    reg = g->gpio_reg + 4*(gpio/10);
    shift = (gpio%10) * 3;

    writel((readl(reg) & ~(7 << shift)) | (mode << shift), reg);
}

static void gpio_set(struct garage_dev *g, unsigned gpio)
{
    void *reg = GPIO_REG_SET(gpio) + g->gpio_reg;

    writel(readl(reg) | GPIO_BIT(gpio), reg);
}

static void gpio_clear(struct garage_dev *g, unsigned gpio)
{
    void *reg = GPIO_REG_CLEAR(gpio) + g->gpio_reg;

    writel(readl(reg) | GPIO_BIT(gpio), reg);
}

static int garage_allocate_resources(struct garage_dev *g)
{
    dma_cap_mask_t mask;
    dma_set_coherent_mask(g->dev, DMA_BIT_MASK(32)); //move to __init

    g->gpio_reg = ioremap(GPIO_BASE, SZ_16K);
    g->pwm_reg = ioremap(PWM_BASE, SZ_16K);
    g->clk_reg = ioremap(CLK_BASE, SZ_16K);
    g->dma_reg = ioremap(DMA_BASE, SZ_16K);

    dma_cap_zero(mask);
    dma_cap_set(DMA_SLAVE, mask);
    g->dma_chan = dma_request_channel(mask, NULL, NULL);
    if(g->dma_chan == NULL) {
        dev_err(g->dev, "error: DMA request channel failed\n");
        return -EIO;
    }

    g->cb_base = dma_alloc_writecombine(g->dev, SZ_64K, &g->cb_handle, GFP_KERNEL);
    if(g->cb_base == NULL) {
        dev_err(g->dev, "error: dma_alloc_writecombine failed\n");
        return -ENOMEM;
    }

    printk(KERN_INFO "Allocated DMA channel %d\n", g->dma_chan->chan_id);

    return 0;
}

static int garage_release_resources(struct garage_dev *g)
{
    gpio_set_mode(g, 18, 1);

    if(g->dma_chan_base) {
        writel(BCM2708_DMA_RESET, g->dma_chan_base + BCM2708_DMA_CS);
    }

    if(g->dma_chan) {
        dma_release_channel(g->dma_chan);
    }

    if(g->cb_base) {
        dma_free_writecombine(g->dev, SZ_64K, g->cb_base, g->cb_handle);
    }

    return 0;
}

static void garage_stop(struct garage_dev *g)
{
    if(g->gpio_reg) {
        gpio_set_mode(g, 18, 1);
    }

    if(g->clk_reg) {
        // stop all the clocks...
        g->clk_reg[PWMCLK_CNTL] = CLK_PASSWD | PLL_500MHZ | CLKCNTL_MASH(0);
    }

    if(g->pwm_reg) {
        writel(CLRF, g->pwm_reg + PWM_CTRL);
    }

    if(g->dma_chan_base) {
        writel(BCM2708_DMA_RESET | BCM2708_DMA_ABORT, g->dma_chan_base + BCM2708_DMA_CS);
        writel(BCM2708_DMA_INT | BIT(1), g->dma_chan_base + BCM2708_DMA_CS); // INT & END
    }
}

static void garage_dma_done(void *data)
{
    struct garage_dev *g = data;
    ktime_t diff = ktime_sub(ktime_get(), g->start_time);

    garage_stop(g);

    printk(KERN_INFO "all done: %ld ms\n", (long)ktime_to_ms(diff));
}

// bcm2835 dmaengine driver does not support interlived transactions.
// Here we use a hack to get an exclusive access to the channel registers,
// while letting dmaengine handle the IRQ for us.
static int start_dummy_tx(struct garage_dev *g) 
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;
    struct dma_slave_config slave_config = {};
    dma_addr_t src_ad;
    int i, err;

    if((err = dmaengine_terminate_all(g->dma_chan)) < 0) {
        dev_err(g->dev, "dmaengine_terminate_all failed\n");
        return err;
    }

    slave_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
    slave_config.dst_addr = 0x7e200028;
    slave_config.src_maxburst = 1;
    slave_config.dst_maxburst = 1;
    slave_config.slave_id = 5;
    slave_config.direction = DMA_MEM_TO_DEV;
    slave_config.device_fc = false;

    if (dmaengine_slave_config(g->dma_chan, &slave_config)) {
        dev_err(g->dev, "failed to configure dma channel\n");
        garage_release_resources(g);
        return -EINVAL;
    }

    sg_init_table(&g->sg, 1); // dummy sg, will be ignored
    sg_dma_address(&g->sg) = g->cb_handle;
    sg_dma_len(&g->sg) = 4;

    // setup a dummy tx, we're only interested in setting up the completion callback
    desc = dmaengine_prep_slave_sg(
            g->dma_chan, 
            &g->sg, 1, 
            DMA_MEM_TO_DEV, 
            DMA_PREP_INTERRUPT);

    if (!desc) {
        dev_err(g->dev, "error: dmaengine_prep_slave_sg failed\n");
        garage_release_resources(g);
        return -EINVAL;
    }

    desc->callback = garage_dma_done;
    desc->callback_param = g;

    // submit tx, while DREQ is inactive, so we can identify 
    // hardware channel number and hack our own CBs
    cookie = dmaengine_submit(desc);

    err = dma_submit_error(cookie);
    if(err) {
        dev_err(g->dev, "error: dmaengine_submit failed\n");
        return err;
    }

    g->dma_chan->device->device_issue_pending(g->dma_chan);

    // guess hw channel number by looking for transfer source addess in DMA registers
    for(i=0;i<15;i++) {
        g->dma_chan_base = g->dma_reg + i*0x100;
        src_ad = readl(g->dma_chan_base + BCM2708_DMA_SOURCE_AD);
        // DMA read is not controled by DREQ, so src address must be already incremeted
        if(src_ad == g->cb_handle + 4) {
            printk(KERN_INFO "Detected hw channel %d.\n", i);
            break;
        }
    }

    if(i == 15) {
        g->dma_chan_base = NULL;
        dev_err(g->dev, "error: failed to identify allocated channel\n");
        return -EINVAL;
    }

    return 0;
}

static void init_pwm_clock(struct garage_dev *g, int freq)
{
    int divi, divf;
    long long tmp;

    divi = GHZ/freq;
    tmp = 0x1000LL*(GHZ%freq);
    do_div(tmp, freq);
    divf = (int) tmp;

    g->clk_reg[PWMCLK_CNTL] = CLK_PASSWD | CLKCNTL_MASH(3) | PLL_1GHZ; // disable clock
    g->clk_reg[PWMCLK_DIV] = CLK_PASSWD | CLKDIV_DIVI(divi) | CLKDIV_DIVF(divf); // set div ratio
    g->clk_reg[PWMCLK_CNTL] = CLK_PASSWD | CLKCNTL_ENAB | CLKCNTL_MASH(3) | PLL_1GHZ; // enable clock
}


static struct bcm2708_dma_cb *add_xfer(struct garage_dev *g, dma_addr_t from, dma_addr_t to, int len)
{
    struct bcm2708_dma_cb *cb = g->cb_base + g->sample;

    if(g->sample > 0) {
        g->cb_base[g->sample-1].next = g->cb_handle + sizeof(*cb)*g->sample;
    }

    cb->info = 
        BCM2708_DMA_WAIT_RESP | 
        BCM2708_DMA_S_INC | 
        BCM2708_DMA_BURST(1) | 
        BIT(26) | // no wide bursts
        0; 
    cb->src = from;
    cb->dst = to;
    cb->length = len;
    cb->stride = 0;
    cb->next = 0;
    cb->pad[0] = 0;
    cb->pad[1] = 0;

    if(g->sample < MAX_CBS) {
        g->sample++;
    } else {
        dev_err(g->dev, "error: out of CBs!\n");
    }

    return cb;
}

static void outbit(struct garage_dev *g, int bit)
{
     // set PWM1 pattern (amplitude)
    add_xfer(g, g->buf_handle+4+(bit ? 4 : 0), PHYS_TO_DMA(PWM_BASE + PWM_DAT1), 4);

    // wait 1 full sample rate period
    add_xfer(g, g->buf_handle+4*3, PHYS_TO_DMA(PWM_BASE + PWM_FIFO), XFR_SZ*4)
        ->info |= BCM2708_DMA_PER_MAP(5) | BCM2708_DMA_D_DREQ; 
}

static int garage_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct garage_dev *g = kzalloc(sizeof(struct garage_dev), GFP_KERNEL);
    u32 *buf;
    int err, i, width;
    const char *p;

    platform_set_drvdata(pdev, g);
    g->dev = dev;
    g->dma_chan_base = NULL;
    g->freq = 40685000; // 40MHz carrier
    g->srate = 1250*XFR_SZ; // 1250Hz = 800us period

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

    writel(CLRF, g->pwm_reg + PWM_CTRL); // stop both channels, clear fifo
    writel(0, g->pwm_reg + PWM_DMAC); // disable DMA

    writel(32, g->pwm_reg + PWM_RNG1); // set PWM1 pattern width to 32 bits
    writel(0, g->pwm_reg + PWM_DAT1); // set initial amplitude to zero (seializing zero)

    writel(width, g->pwm_reg + PWM_RNG2); // set period to 1/2 seconds
    // enable channels:
    // PWM1 - 32bit serializer mode, no FIFO, repeat
    // PWM2 - M/S mode, FIFO, no repeat
    writel(CLRF | MODE1 | PWEN1 | RPTL1 | MSEN2 | PWEN2 | USEF2, g->pwm_reg + PWM_CTRL);

    if((err = start_dummy_tx(g)) < 0) {
        garage_release_resources(g);
        return err;
    }

    // setup buffer
    buf[0] = GPIO_BIT(BUSY_LED_PIN);       // busy led pin
    buf[1] = 0;             // amplitude == 0
    buf[2] = 0xaaaaaaaa;    // amplitude == max (1010101010...1010b)
    for(i=0;i<XFR_SZ;i++) {
        buf[i+3] = width/2; // don't care, but could be useful for debugging
    }

    g->sample = 0;

    for(p=code;*p;p++) {
        outbit(g, *p == '1');
    }

    add_xfer(g, g->buf_handle, PHYS_TO_DMA(GPIO_BASE + GPIO_REG_CLEAR(BUSY_LED_PIN)), 4)
        ->info |= BCM2708_DMA_INT_EN;

    writel(BCM2708_DMA_RESET | BCM2708_DMA_ABORT, g->dma_chan_base + BCM2708_DMA_CS);
    writel(BCM2708_DMA_INT | BIT(1), g->dma_chan_base + BCM2708_DMA_CS); // INT & END

    bcm_dma_start(g->dma_chan_base, g->cb_handle);
    g->start_time = ktime_get();

    // clear FIFO, restart PWM
    writel(CLRF | MODE1 | PWEN1 | RPTL1 | MSEN2 | PWEN2 | USEF2, g->pwm_reg + PWM_CTRL);
    writel(0x80000001, g->pwm_reg + PWM_DMAC); // enable DMA, 1 word threshold

    return 0;
}

static int garage_remove(struct platform_device *pdev)
{
    struct garage_dev *g = platform_get_drvdata(pdev);

    gpio_clear(g, BUSY_LED_PIN);

    garage_stop(g);

    garage_release_resources(g);

    iounmap(g->gpio_reg);
    iounmap(g->pwm_reg);
    iounmap(g->clk_reg);
    iounmap(g->dma_reg);

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

