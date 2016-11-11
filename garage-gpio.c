
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/timekeeping.h>

#include "garage-driver.h"
#include "garage-gpio.h"

void gpio_set_mode(struct garage_dev *g, unsigned gpio, unsigned mode)
{
    int shift;
    void *reg;

    reg = g->gpio_reg + 4*(gpio/10);
    shift = (gpio%10) * 3;

    writel((readl(reg) & ~(7 << shift)) | (mode << shift), reg);
}

void gpio_set(struct garage_dev *g, unsigned gpio)
{
    void *reg = GPIO_REG_SET(gpio) + g->gpio_reg;

    writel(readl(reg) | GPIO_BIT(gpio), reg);
}

void gpio_clear(struct garage_dev *g, unsigned gpio)
{
    void *reg = GPIO_REG_CLEAR(gpio) + g->gpio_reg;

    writel(readl(reg) | GPIO_BIT(gpio), reg);
}
