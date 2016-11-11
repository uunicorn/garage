
#ifndef __GARAGE_DMA_H__
#define __GARAGE_DMA_H__

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/platform_data/dma-bcm2708.h>

#define PHYS_TO_DMA(x)  (0x7E000000 - BCM2708_PERI_BASE + x)

// reserve this number of DMA control blocks (limits the maximum length of code sequence)
#define MAX_CBS 600

struct garage_dev;

int dma_allocate(struct garage_dev *g);
void dma_release(struct garage_dev *g);
void dma_reset(struct garage_dev *g);
int start_dummy_tx(struct garage_dev *g);
struct bcm2708_dma_cb *add_xfer(struct garage_dev *g, dma_addr_t from, dma_addr_t to, int len);

#endif
