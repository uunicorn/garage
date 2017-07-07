
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_SIZE (4*1024)

#define BCM2708_PERI_BASE	0x3F000000
#define CLK_BASE                (BCM2708_PERI_BASE + 0x00101000)
#define CLK_LEN                 0xA8
#define GPIO_BASE               (BCM2708_PERI_BASE + 0x00200000)
#define GPIO_LEN                0xB4

#define GPCLK_CNTL              (0x70/4)
#define GPCLK_DIV               (0x74/4)

#define ANTENNA_PIN		4

#define PLL_1GHZ		0x5

volatile uint32_t *gpio_reg;


void *
mapmem(unsigned base, unsigned size)
{
    int mem_fd;
    unsigned offset = base % PAGE_SIZE;
    base = base - offset;
    
    if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
        perror("open: /dev/mem");
	exit(-1);
    }

    void *mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, base);

    if (mem == MAP_FAILED) {
	perror("mmap");
	exit(-1);
    }

    close(mem_fd);

    return (char *)mem + offset;
}

void
init_gpio()
{
    gpio_reg = mapmem(GPIO_BASE, GPIO_LEN);
}

void
gpio_setmode(unsigned gpio, unsigned mode)
{
    int reg = gpio/10;
    int shift = (gpio%10)*3;

    gpio_reg[reg] = (gpio_reg[reg] & ~(7<<shift)) | (mode<<shift);
}

void
init_clk()
{
    double freq = 40.685e6;
    unsigned char mash = 3;
    int divi, divf;
    volatile uint32_t *clk_reg = mapmem(CLK_BASE, CLK_LEN);

    divi = 1e9/freq;
    divf = (1e9/freq - divi)*0x1000;

    gpio_setmode(ANTENNA_PIN, 0);

    clk_reg[GPCLK_CNTL] = 0x5A000000 | (mash << 9) | PLL_1GHZ;
    usleep(300);
    clk_reg[GPCLK_DIV] = 0x5A000000 | (divi << 12) | divf;
    usleep(300);
    clk_reg[GPCLK_CNTL] = 0x5A000010 | (mash << 9) | PLL_1GHZ;
    usleep(300);
}

void
outbit(int b)
{
    gpio_setmode(ANTENNA_PIN, b ? 4 : 0); // Clock out or GPO?
    usleep(700);
}

void
outtriplet(int b)
{
    outbit(1);
    outbit(0);
    outbit(!b);
}

int
main(int argc, const char **argv)
{
    int i;
    const char *code, *p;

    if(argc != 2) {
        fprintf(stderr, "Usage: %s <door combination>\n", argv[0]);
	exit(-1);
    }

    code = argv[1];

    init_gpio();
    init_clk();

    outbit(1);
    outbit(1);
    outbit(1);
    outbit(1);

    outtriplet(0);

    for(i=0;i<5;i++) {
        outtriplet(0);
        for(p=code;*p;p++) {
            outtriplet(*p == '1');
        }
        outtriplet(0);
        outbit(1);
        outbit(1);
        outbit(1);
        outbit(1);
        outbit(1);
    }
    outbit(0);

    gpio_setmode(ANTENNA_PIN, 0);

    return 0;
}
