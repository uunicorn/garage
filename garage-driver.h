
#ifndef __GARAGE_DRIVER_H__
#define __GARAGE_DRIVER_H__

#define BUSY_LED_PIN 19


struct garage_dev {
    struct device *dev;

    void *pwm_reg, *dma_reg, *dma_chan_base, *gpio_reg, *clk_reg;

    struct dma_chan *dma_chan;
    struct bcm2708_dma_cb *cb_base;		/* DMA control blocks */
    dma_addr_t cb_handle, buf_handle;
    int sample;
    int freq;
    int srate;
    ktime_t start_time;
    wait_queue_head_t wq;
    int done;
};


void garage_dma_done(void *data);

#endif
