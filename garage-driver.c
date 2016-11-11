
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
#include "garage-clk.h"

#define DRVNAME "garage-door"

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
    if(g->gpio_reg)
        gpio_set_mode(g, 18, 1);

    if(g->clk_reg)
        pwm_clock_stop(g);

    if(g->pwm_reg)
        pwm_stop(g);

    if(g->dma_chan_base)
        dma_reset(g);
}

void garage_dma_done(void *data)
{
    struct garage_dev *g = data;
    ktime_t diff = ktime_sub(ktime_get(), g->start_time);

    garage_stop(g);

    g->done = 1;
    wake_up_interruptible(&g->wq);

    printk(KERN_INFO "all done: %ld ms\n", (long)ktime_to_ms(diff));
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
    int err;

    platform_set_drvdata(pdev, g);
    g->dev = dev;
    g->dma_chan_base = NULL;
    g->freq = 0;
    g->srate = 0;
    init_waitqueue_head(&g->wq);

    if((err = garage_allocate_resources(g)) < 0) {
        garage_release_resources(g);
        return err;
    }

    return 0;
}

static int garage_remove(struct platform_device *pdev)
{
    struct garage_dev *g = platform_get_drvdata(pdev);

    gpio_clear(g, BUSY_LED_PIN);

    garage_stop(g);

    garage_release_resources(g);

    platform_set_drvdata(pdev, NULL);

    printk(KERN_INFO "Goodbye world.\n");

    return 0;
}

static int send_sequence(struct garage_dev *g, const char *code)
{
    const char *p;
    int err;

    g->done = 0;
    gpio_set_mode(g, 18, 2);                // pin18 -> PWM out
    gpio_set_mode(g, BUSY_LED_PIN, 1);      // GPIO out (busy led)
    gpio_set(g, BUSY_LED_PIN);              // busy led ON

    // set PWM clock to 2x carrier frequency 
    // (2x, because 101010...1010b serializer pattern divides clock frequency by two)
    pwm_clock_init(g, g->freq*2);

    pwm_stop(g);

    pwm_init(g, 0); // start PWM, but keep DREQ low

    if((err = start_dummy_tx(g)) < 0) {
        garage_stop(g);
        return err;
    }

    g->sample = 0;

    for(p=code;*p;p++) {
        switch(*p) {
            case '1':
                outbit(g, 1);
                break;
            case '0':
                outbit(g, 0);
                break;
            default:
                /* ignore */ ;
        }
    }

    add_xfer(g, g->buf_handle, PHYS_TO_DMA(GPIO_BASE + GPIO_REG_CLEAR(BUSY_LED_PIN)), 4)
        ->info |= BCM2708_DMA_INT_EN;

    dma_reset(g);

    bcm_dma_start(g->dma_chan_base, g->cb_handle);
    g->start_time = ktime_get();

    pwm_init(g, 1); // restart PWM, enable DMA

    return 0;
}

static ssize_t carrier_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct garage_dev *g = dev_get_drvdata(dev);

    if(g == NULL) {
        dev_err(g->dev, "error: garage driver not loaded\n");
        return -EINVAL;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", g->freq);
}

static ssize_t carrier_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    char *end;
    long new = simple_strtol(buf, &end, 0);
    struct garage_dev *g = dev_get_drvdata(dev);
    
    if (end == buf || new > INT_MAX || new < INT_MIN) {
        dev_err(g->dev, "error: int number expected for carrier attribute\n");
        return -EINVAL;
    }

    if(g == NULL) {
        dev_err(g->dev, "error: garage driver not loaded\n");
        return -EINVAL;
    }

    g->freq = (long)new;

    return count;
}


static ssize_t srate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct garage_dev *g = dev_get_drvdata(dev);

    if(g == NULL) {
        dev_err(g->dev, "error: garage driver not loaded\n");
        return -EINVAL;
    }

    return scnprintf(buf, PAGE_SIZE, "%d\n", g->srate);
}

static ssize_t srate_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    char *end;
    long new = simple_strtol(buf, &end, 0);
    struct garage_dev *g = dev_get_drvdata(dev);
    
    if (end == buf || new > INT_MAX || new < INT_MIN) {
        dev_err(g->dev, "error: int number expected for srate attribute\n");
        return -EINVAL;
    }

    if(g == NULL) {
        dev_err(g->dev, "error: garage driver not loaded\n");
        return -EINVAL;
    }

    g->srate = (long)new;

    return count;
}

static ssize_t sequence_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct garage_dev *g = dev_get_drvdata(dev);
    int err;

    if(g == NULL) {
        dev_err(g->dev, "error: garage driver not loaded\n");
        return -EINVAL;
    }

    err = send_sequence(g, buf);
    if(err < 0)
        return err;

    err = wait_event_interruptible(g->wq, g->done);
    if(err < 0)
        return err;

    return count;
}

DEVICE_ATTR(carrier, 0644, carrier_show, carrier_store);
DEVICE_ATTR(srate, 0644, srate_show, srate_store);
DEVICE_ATTR(sequence, 0644, NULL, sequence_store);

static struct attribute *dev_attrs[] = {
    &dev_attr_carrier.attr,
    &dev_attr_srate.attr,
    &dev_attr_sequence.attr,
    NULL,
};

static struct attribute_group dev_attr_group = {
    .attrs = dev_attrs,
};

static const struct attribute_group *dev_attr_groups[] = {
    &dev_attr_group,
    NULL,
};

static struct platform_driver garage_driver = {
    .probe    = garage_probe,
    .remove   = garage_remove,
    .driver   = {
        .name = DRVNAME,
    },
};


static void platform_device_release(struct device *dev)
{
}

static struct platform_device garage_device = {
    .name = DRVNAME,
    .id = -1,
    .resource = NULL,
    .num_resources = 0,
    .dev = {
        .release = platform_device_release,
        .groups = dev_attr_groups,
        .coherent_dma_mask = DMA_BIT_MASK(32),
    },
};


static int __init garage_init(void)
{
    int ret;

    ret = platform_driver_register(&garage_driver);
    if(ret < 0)
        return ret;

    ret = platform_device_register(&garage_device);
    if(ret < 0) {
        platform_driver_unregister(&garage_driver);
        return ret;
    }

    return 0;
}

module_init(garage_init);

static void __exit garage_exit(void)
{
    platform_device_unregister(&garage_device);
    platform_driver_unregister(&garage_driver);
}

module_exit(garage_exit);

MODULE_LICENSE("GPL");

