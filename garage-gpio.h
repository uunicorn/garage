
#ifndef __GARAGE_GPIO_H__
#define __GARAGE_GPIO_H__


#define GPIO_REG_SET(x)     (x < 32 ? 0x1c : 0x20)
#define GPIO_REG_CLEAR(x)   (x < 32 ? 0x28 : 0x2c)
#define GPIO_BIT(x)         BIT(x < 32 ? x : (x - 32))

struct garage_dev;

extern void gpio_clear(struct garage_dev *g, unsigned gpio);
extern void gpio_set(struct garage_dev *g, unsigned gpio);
extern void gpio_set_mode(struct garage_dev *g, unsigned gpio, unsigned mode);

#endif
