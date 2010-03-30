/*
 * PB926 platform-specific initialisation and setup
 *
 * Copyright (C) 2007 Bahadir Balban
 */
#include <l4/generic/platform.h>
#include <l4/generic/space.h>
#include <l4/generic/irq.h>
#include <l4/generic/bootmem.h>
#include INC_ARCH(linker.h)
#include INC_SUBARCH(mm.h)
#include INC_SUBARCH(mmu_ops.h)
#include INC_GLUE(memory.h)
#include INC_GLUE(mapping.h)
#include INC_GLUE(memlayout.h)
#include INC_PLAT(offsets.h)
#include INC_PLAT(platform.h)
#include INC_PLAT(uart.h)
#include INC_PLAT(timer.h)
#include INC_PLAT(irq.h)
#include INC_ARCH(asm.h)
#include INC_ARCH(io.h)
#include <l4/generic/capability.h>
#include <l4/generic/cap-types.h>

/*
 * The devices that are used by the kernel are mapped
 * independent of these capabilities, but these provide a
 * concise description of what is used by the kernel.
 */
int platform_setup_device_caps(struct kernel_resources *kres)
{
	struct capability *uart[3], *timer[1],
			  *keyboard[1], *mouse[1];

	/* Setup capabilities for userspace uarts and timers */
	uart[0] =  alloc_bootmem(sizeof(*uart[0]), 0);
	uart[0]->start = __pfn(PLATFORM_UART1_BASE);
	uart[0]->end = uart[0]->start + 1;
	uart[0]->size = uart[0]->end - uart[1]->start;
	cap_set_devtype(uart[0], CAP_DEVTYPE_UART);
	cap_set_devnum(uart[0], 1);
	link_init(&uart[0]->list);
	cap_list_insert(uart[0], &kres->devmem_free);

	uart[1] =  alloc_bootmem(sizeof(*uart[1]), 0);
	uart[1]->start = __pfn(PLATFORM_UART2_BASE);
	uart[1]->end = uart[1]->start + 1;
	uart[1]->size = uart[1]->end - uart[1]->start;
	cap_set_devtype(uart[1], CAP_DEVTYPE_UART);
	cap_set_devnum(uart[1], 2);
	link_init(&uart[1]->list);
	cap_list_insert(uart[1], &kres->devmem_free);

	uart[2] =  alloc_bootmem(sizeof(*uart[2]), 0);
	uart[2]->start = __pfn(PLATFORM_UART3_BASE);
	uart[2]->end = uart[2]->start + 1;
	uart[2]->size = uart[2]->end - uart[2]->start;
	cap_set_devtype(uart[2], CAP_DEVTYPE_UART);
	cap_set_devnum(uart[2], 3);
	link_init(&uart[2]->list);
	cap_list_insert(uart[2], &kres->devmem_free);

	/* Setup timer1 capability as free */
	timer[0] =  alloc_bootmem(sizeof(*timer[0]), 0);
	timer[0]->start = __pfn(PLATFORM_TIMER1_BASE);
	timer[0]->end = timer[0]->start + 1;
	timer[0]->size = timer[0]->end - timer[0]->start;
	cap_set_devtype(timer[0], CAP_DEVTYPE_TIMER);
	cap_set_devnum(timer[0], 1);
	link_init(&timer[0]->list);
	cap_list_insert(timer[0], &kres->devmem_free);

        /* Setup keyboard capability as free */
        keyboard[0] =  alloc_bootmem(sizeof(*keyboard[0]), 0);
        keyboard[0]->start = __pfn(PLATFORM_KEYBOARD0_BASE);
        keyboard[0]->end = keyboard[0]->start + 1;
        keyboard[0]->size = keyboard[0]->end - keyboard[0]->start;
        cap_set_devtype(keyboard[0], CAP_DEVTYPE_KEYBOARD);
        cap_set_devnum(keyboard[0], 0);
        link_init(&keyboard[0]->list);
        cap_list_insert(keyboard[0], &kres->devmem_free);

        /* Setup mouse capability as free */
        mouse[0] =  alloc_bootmem(sizeof(*mouse[0]), 0);
        mouse[0]->start = __pfn(PLATFORM_MOUSE0_BASE);
        mouse[0]->end = mouse[0]->start + 1;
        mouse[0]->size = mouse[0]->end - mouse[0]->start;
        cap_set_devtype(mouse[0], CAP_DEVTYPE_MOUSE);
        cap_set_devnum(mouse[0], 0);
        link_init(&mouse[0]->list);
        cap_list_insert(mouse[0], &kres->devmem_free);

	return 0;
}

/*
 * We will use UART0 for kernel as well as user tasks,
 * so map it to kernel and user space
 */
void init_platform_console(void)
{
	add_boot_mapping(PLATFORM_UART0_BASE, PLATFORM_CONSOLE_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

	/*
	 * Map same UART IO area to userspace so that primitive uart-based
	 * userspace printf can work. Note, this raw mapping is to be
	 * removed in the future, when file-based io is implemented.
	 */
	add_boot_mapping(PLATFORM_UART0_BASE, USERSPACE_CONSOLE_VBASE,
			 PAGE_SIZE, MAP_USR_IO);

	uart_init(PLATFORM_CONSOLE_VBASE);
}

/*
 * We are using TIMER0 only, so we map TIMER0 base,
 * incase any other timer is needed we need to map it
 * to userspace or kernel space as needed
 */
void init_platform_timer(void)
{
	add_boot_mapping(PLATFORM_TIMER0_BASE, PLATFORM_TIMER0_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

	/* 1 Mhz means can tick up to 1,000,000 times a second */
	timer_init(PLATFORM_TIMER0_VBASE, 1000000 / CONFIG_SCHED_TICKS);
}

void init_platform_irq_controller()
{
	add_boot_mapping(PLATFORM_VIC_BASE, PLATFORM_IRQCTRL0_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);
	add_boot_mapping(PLATFORM_SIC_BASE, PLATFORM_IRQCTRL1_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);
	irq_controllers_init();
}

/* Add userspace devices here as you develop their irq handlers */
void init_platform_devices()
{
	/* TIMER23 */
	add_boot_mapping(PLATFORM_TIMER1_BASE, PLATFORM_TIMER1_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

	/* KEYBOARD - KMI0 */
	add_boot_mapping(PLATFORM_KEYBOARD0_BASE, PLATFORM_KEYBOARD0_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

	/* MOUSE - KMI1 */
	add_boot_mapping(PLATFORM_MOUSE0_BASE, PLATFORM_MOUSE0_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

}

/* If these bits are off, 32Khz OSC source is used */
#define TIMER3_SCTRL_1MHZ	(1 << 21)
#define TIMER2_SCTRL_1MHZ	(1 << 19)
#define TIMER1_SCTRL_1MHZ	(1 << 17)
#define TIMER0_SCTRL_1MHZ	(1 << 15)

/* Set all timers to use 1Mhz OSC clock */
void init_timer_osc(void)
{
	volatile u32 reg;

	add_boot_mapping(PLATFORM_SYSCTRL_BASE, PLATFORM_SYSCTRL_VBASE,
			 PAGE_SIZE, MAP_IO_DEFAULT);

	reg = read(SP810_SCCTRL);

	write(reg | TIMER0_SCTRL_1MHZ | TIMER1_SCTRL_1MHZ
	      | TIMER2_SCTRL_1MHZ | TIMER3_SCTRL_1MHZ,
	      SP810_SCCTRL);
}

void platform_timer_start(void)
{
	/* Enable irq line for TIMER0 */
	irq_enable(IRQ_TIMER0);

	/* Enable timer */
	timer_start(PLATFORM_TIMER0_VBASE);
}

void platform_init(void)
{
	init_timer_osc();
	init_platform_console();
	init_platform_timer();
	init_platform_irq_controller();
	init_platform_devices();
}

