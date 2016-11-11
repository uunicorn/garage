
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/timekeeping.h>
#include <linux/interrupt.h>

#include "garage-driver.h"
#include "garage-dma.h"
#include "garage-gpio.h"


int dma_allocate(struct garage_dev *g)
{
    dma_cap_mask_t mask;
    u32 *buf;

    g->dma_reg = ioremap(DMA_BASE, SZ_16K);

    if(g->dma_reg == NULL) {
        dev_err(g->dev, "error: failed to ioremap DMA registers\n");
        return -ENOMEM;
    }

    dma_cap_zero(mask);
    dma_cap_set(DMA_SLAVE, mask);
    g->dma_chan = dma_request_channel(mask, NULL, NULL);
    if(g->dma_chan == NULL) {
        dev_err(g->dev, "error: DMA request channel failed\n");
        return -EIO;
    }

    g->cb_base = dma_alloc_writecombine(g->dev, sizeof(*g->cb_base)*MAX_CBS + 4*4, &g->cb_handle, GFP_KERNEL);
    if(g->cb_base == NULL) {
        dev_err(g->dev, "error: dma_alloc_writecombine failed\n");
        return -ENOMEM;
    }

    buf = (u32*)(g->cb_base + MAX_CBS);
    g->buf_handle = g->cb_handle + sizeof(*g->cb_base)*MAX_CBS;

    // setup the buffer
    buf[0] = GPIO_BIT(BUSY_LED_PIN);       // busy led pin
    buf[1] = 0;             // amplitude == 0
    buf[2] = 0xaaaaaaaa;    // amplitude == max (1010101010...1010b)
    buf[3] = 0;             // carrier to sample rate ratio is unknown yet. Set to half of PWM_RNG2 for debugging.

    printk(KERN_INFO "Allocated DMA channel %d\n", g->dma_chan->chan_id);

    return 0;
}

void dma_release(struct garage_dev *g)
{
    if(g->dma_chan_base)
        dma_reset(g);

    if(g->dma_chan)
        dma_release_channel(g->dma_chan);

    if(g->cb_base)
        dma_free_writecombine(g->dev, sizeof(*g->cb_base)*MAX_CBS + 4*4, g->cb_base, g->cb_handle);

    if(g->dma_reg)
        iounmap(g->dma_reg);
}

void dma_reset(struct garage_dev *g)
{
    writel(BCM2708_DMA_RESET | BCM2708_DMA_ABORT, g->dma_chan_base + BCM2708_DMA_CS);
    writel(BCM2708_DMA_INT | BIT(1), g->dma_chan_base + BCM2708_DMA_CS); // clear INT & END
}

// bcm2835 dmaengine driver does not support interlived transactions.
// Here we use a hack to get an exclusive access to the channel registers,
// while letting dmaengine handle the IRQ for us.
int start_dummy_tx(struct garage_dev *g)
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;
    struct dma_slave_config slave_config = {};
    dma_addr_t src_ad;
    int i, err;
    struct scatterlist sg;

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
        return -EINVAL;
    }

    sg_init_table(&sg, 1); // dummy sg, will be ignored
    sg_dma_address(&sg) = g->cb_handle;
    sg_dma_len(&sg) = 4;

    // setup a dummy tx, we're only interested in setting up the completion callback
    desc = dmaengine_prep_slave_sg(
            g->dma_chan, 
            &sg, 1, 
            DMA_MEM_TO_DEV, 
            DMA_PREP_INTERRUPT);

    if (!desc) {
        dev_err(g->dev, "error: dmaengine_prep_slave_sg failed\n");
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

struct bcm2708_dma_cb *add_xfer(struct garage_dev *g, dma_addr_t from, dma_addr_t to, int len)
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

