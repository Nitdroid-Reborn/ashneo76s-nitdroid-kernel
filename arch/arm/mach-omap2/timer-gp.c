/*
 * linux/arch/arm/mach-omap2/timer-gp.c
 *
 * OMAP2 GP timer support.
 *
 * Update to use new clocksource/clockevent layers
 * Author: Kevin Hilman, MontaVista Software, Inc. <source@mvista.com>
 * Copyright (C) 2007 MontaVista Software, Inc.
 *
 * Original driver:
 * Copyright (C) 2005 Nokia Corporation
 * Author: Paul Mundt <paul.mundt@nokia.com>
 *         Juha Yrjölä <juha.yrjola@nokia.com>
 * OMAP Dual-mode timer framework support by Timo Teras
 *
 * Some parts based off of TI's 24xx code:
 *
 *   Copyright (C) 2004 Texas Instruments, Inc.
 *
 * Roughly modelled after the OMAP1 MPU timer code.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>

#include <asm/mach/time.h>
#include <mach/dmtimer.h>

static struct omap_dm_timer *gptimer;
static struct clock_event_device clockevent_gpt;

static irqreturn_t omap2_gp_timer_interrupt(int irq, void *dev_id)
{
	struct omap_dm_timer *gpt = (struct omap_dm_timer *)dev_id;
	struct clock_event_device *evt = &clockevent_gpt;

	omap_dm_timer_write_status(gpt, OMAP_TIMER_INT_OVERFLOW);

	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static struct irqaction omap2_gp_timer_irq = {
	.name		= "gp timer",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= omap2_gp_timer_interrupt,
};

static int omap2_gp_timer_set_next_event(unsigned long cycles,
					 struct clock_event_device *evt)
{
	omap_dm_timer_set_load_start(gptimer, 0, 0xffffffff - cycles);

	return 0;
}

static void omap2_gp_timer_set_mode(enum clock_event_mode mode,
				    struct clock_event_device *evt)
{
	u32 period;

	omap_dm_timer_stop(gptimer);

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		period = clk_get_rate(omap_dm_timer_get_fclk(gptimer)) / HZ;
		period -= 1;

		omap_dm_timer_set_load_start(gptimer, 1, 0xffffffff - period);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		break;
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_RESUME:
		break;
	case CLOCK_EVT_MODE_SHUTDOWN:
		/*
		 * Wait for min period x 2 to make sure that timer is
		 * stopped
		 */
		udelay(evt->min_delta_ns / 500);
		/*
		 * Clear possibly pending interrupt, this will occasionally
		 * generate spurious timer IRQs during suspend but this
		 * is okay, as another option is not to enter suspend at all
		 */
		omap_dm_timer_write_status(gptimer, OMAP_TIMER_INT_OVERFLOW);
		break;
	}
}

static struct clock_event_device clockevent_gpt = {
	.name		= "gp timer",
	.features       = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.shift		= 32,
	.set_next_event	= omap2_gp_timer_set_next_event,
	.set_mode	= omap2_gp_timer_set_mode,
};

static void __init omap2_gp_clockevent_init(void)
{
	u32 tick_rate;

	gptimer = omap_dm_timer_request_specific(CONFIG_OMAP_TICK_GPTIMER);
	BUG_ON(gptimer == NULL);

#if defined(CONFIG_OMAP_32K_TIMER)
	omap_dm_timer_set_source(gptimer, OMAP_TIMER_SRC_32_KHZ);
#else
	omap_dm_timer_set_source(gptimer, OMAP_TIMER_SRC_SYS_CLK);
#endif
	tick_rate = clk_get_rate(omap_dm_timer_get_fclk(gptimer));

	pr_info("OMAP clockevent source: GPTIMER%d at %u Hz\n",
		CONFIG_OMAP_TICK_GPTIMER, tick_rate);

	omap2_gp_timer_irq.dev_id = (void *)gptimer;
	setup_irq(omap_dm_timer_get_irq(gptimer), &omap2_gp_timer_irq);
	omap_dm_timer_set_int_enable(gptimer, OMAP_TIMER_INT_OVERFLOW);

	clockevent_gpt.mult = div_sc(tick_rate, NSEC_PER_SEC,
				     clockevent_gpt.shift);
	clockevent_gpt.max_delta_ns =
		clockevent_delta2ns(0xffffffff, &clockevent_gpt);
	clockevent_gpt.min_delta_ns =
		clockevent_delta2ns(3, &clockevent_gpt);
		/* Timer internal resynch latency. */

	clockevent_gpt.cpumask = cpumask_of_cpu(0);
	clockevents_register_device(&clockevent_gpt);
}

#ifdef CONFIG_OMAP_32K_TIMER
/* 
 * When 32k-timer is enabled, don't use GPTimer for clocksource
 * instead, just leave default clocksource which uses the 32k
 * sync counter.  See clocksource setup in see plat-omap/common.c. 
 */

static inline void __init omap2_gp_clocksource_init(void) {}
#else
/*
 * clocksource
 */
static struct omap_dm_timer *gpt_clocksource;
static cycle_t clocksource_read_cycles(void)
{
	return (cycle_t)omap_dm_timer_read_counter(gpt_clocksource);
}

static struct clocksource clocksource_gpt = {
	.name		= "gp timer",
	.rating		= 300,
	.read		= clocksource_read_cycles,
	.mask		= CLOCKSOURCE_MASK(32),
	.shift		= 24,
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

/* Setup free-running counter for clocksource */
static void __init omap2_gp_clocksource_init(void)
{
	static struct omap_dm_timer *gpt;
	u32 tick_rate, tick_period;
	static char err1[] __initdata = KERN_ERR
		"%s: failed to request dm-timer\n";
	static char err2[] __initdata = KERN_ERR
		"%s: can't register clocksource!\n";

	gpt = omap_dm_timer_request();
	if (!gpt)
		printk(err1, clocksource_gpt.name);
	gpt_clocksource = gpt;

	omap_dm_timer_set_source(gpt, OMAP_TIMER_SRC_SYS_CLK);
	tick_rate = clk_get_rate(omap_dm_timer_get_fclk(gpt));
	tick_period = (tick_rate / HZ) - 1;

	omap_dm_timer_set_load_start(gpt, 1, 0);

	clocksource_gpt.mult =
		clocksource_khz2mult(tick_rate/1000, clocksource_gpt.shift);
	if (clocksource_register(&clocksource_gpt))
		printk(err2, clocksource_gpt.name);
}
#endif

static void __init omap2_gp_timer_init(void)
{
	omap_dm_timer_init();

	omap2_gp_clockevent_init();
	omap2_gp_clocksource_init();
}

struct sys_timer omap_timer = {
	.init	= omap2_gp_timer_init,
};
