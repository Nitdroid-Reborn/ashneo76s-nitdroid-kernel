/*
 * linux/drivers/video/omap2/dss/rfbi.c
 *
 * Copyright (C) 2009 Nokia Corporation
 * Author: Tomi Valkeinen <tomi.valkeinen@nokia.com>
 *
 * Some code and ideas taken from drivers/video/omap/ driver
 * by Imre Deak.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DSS_SUBSYS_NAME "RFBI"

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/seq_file.h>

#include <mach/board.h>
#include <mach/display.h>
#include "dss.h"

/*#define MEASURE_PERF*/

#define RFBI_BASE               0x48050800

struct rfbi_reg { u16 idx; };

#define RFBI_REG(idx)		((const struct rfbi_reg) { idx })

#define RFBI_REVISION		RFBI_REG(0x0000)
#define RFBI_SYSCONFIG		RFBI_REG(0x0010)
#define RFBI_SYSSTATUS		RFBI_REG(0x0014)
#define RFBI_CONTROL		RFBI_REG(0x0040)
#define RFBI_PIXEL_CNT		RFBI_REG(0x0044)
#define RFBI_LINE_NUMBER	RFBI_REG(0x0048)
#define RFBI_CMD		RFBI_REG(0x004c)
#define RFBI_PARAM		RFBI_REG(0x0050)
#define RFBI_DATA		RFBI_REG(0x0054)
#define RFBI_READ		RFBI_REG(0x0058)
#define RFBI_STATUS		RFBI_REG(0x005c)

#define RFBI_CONFIG(n)		RFBI_REG(0x0060 + (n)*0x18)
#define RFBI_ONOFF_TIME(n)	RFBI_REG(0x0064 + (n)*0x18)
#define RFBI_CYCLE_TIME(n)	RFBI_REG(0x0068 + (n)*0x18)
#define RFBI_DATA_CYCLE1(n)	RFBI_REG(0x006c + (n)*0x18)
#define RFBI_DATA_CYCLE2(n)	RFBI_REG(0x0070 + (n)*0x18)
#define RFBI_DATA_CYCLE3(n)	RFBI_REG(0x0074 + (n)*0x18)

#define RFBI_VSYNC_WIDTH	RFBI_REG(0x0090)
#define RFBI_HSYNC_WIDTH	RFBI_REG(0x0094)

#define RFBI_CMD_FIFO_LEN_BYTES (16 * sizeof(struct update_param))

#define REG_FLD_MOD(idx, val, start, end) \
	rfbi_write_reg(idx, FLD_MOD(rfbi_read_reg(idx), val, start, end))

/* To work around an RFBI transfer rate limitation */
#define OMAP_RFBI_RATE_LIMIT    1

enum omap_rfbi_cycleformat {
	OMAP_DSS_RFBI_CYCLEFORMAT_1_1 = 0,
	OMAP_DSS_RFBI_CYCLEFORMAT_2_1 = 1,
	OMAP_DSS_RFBI_CYCLEFORMAT_3_1 = 2,
	OMAP_DSS_RFBI_CYCLEFORMAT_3_2 = 3,
};

enum omap_rfbi_datatype {
	OMAP_DSS_RFBI_DATATYPE_12 = 0,
	OMAP_DSS_RFBI_DATATYPE_16 = 1,
	OMAP_DSS_RFBI_DATATYPE_18 = 2,
	OMAP_DSS_RFBI_DATATYPE_24 = 3,
};

enum omap_rfbi_parallelmode {
	OMAP_DSS_RFBI_PARALLELMODE_8 = 0,
	OMAP_DSS_RFBI_PARALLELMODE_9 = 1,
	OMAP_DSS_RFBI_PARALLELMODE_12 = 2,
	OMAP_DSS_RFBI_PARALLELMODE_16 = 3,
};

enum update_cmd {
	RFBI_CMD_UPDATE = 0,
	RFBI_CMD_SYNC   = 1,
};

static int rfbi_convert_timings(struct rfbi_timings *t);
static void rfbi_get_clk_info(u32 *clk_period, u32 *max_clk_div);
static void process_cmd_fifo(void);

static struct {
	void __iomem	*base;

	unsigned long	l4_khz;

	enum omap_rfbi_datatype datatype;
	enum omap_rfbi_parallelmode parallelmode;

	enum omap_rfbi_te_mode te_mode;
	int te_enabled;

	void (*framedone_callback)(void *data);
	void *framedone_callback_data;

	struct omap_display *display[2];

	struct kfifo      *cmd_fifo;
	spinlock_t        cmd_lock;
	struct completion cmd_done;
	atomic_t          cmd_fifo_full;
	atomic_t          cmd_pending;
#ifdef MEASURE_PERF
	unsigned perf_bytes;
	ktime_t perf_setup_time;
	ktime_t perf_start_time;
#endif
} rfbi;

struct update_region {
	u16	x;
	u16     y;
	u16     w;
	u16     h;
};

struct update_param {
	u8 rfbi_module;
	u8 cmd;

	union {
		struct update_region r;
		struct completion *sync;
	} par;
};

static inline void rfbi_write_reg(const struct rfbi_reg idx, u32 val)
{
	__raw_writel(val, rfbi.base + idx.idx);
}

static inline u32 rfbi_read_reg(const struct rfbi_reg idx)
{
	return __raw_readl(rfbi.base + idx.idx);
}

static void rfbi_enable_clocks(bool enable)
{
	if (enable)
		dss_clk_enable(DSS_CLK_ICK | DSS_CLK_FCK1);
	else
		dss_clk_disable(DSS_CLK_ICK | DSS_CLK_FCK1);
}

void omap_rfbi_write_command(const void *buf, u32 len)
{
	rfbi_enable_clocks(1);
	switch (rfbi.parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		const u8 *b = buf;
		for (; len; len--)
			rfbi_write_reg(RFBI_CMD, *b++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		const u16 *w = buf;
		BUG_ON(len & 1);
		for (; len; len -= 2)
			rfbi_write_reg(RFBI_CMD, *w++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();
	}
	rfbi_enable_clocks(0);
}
EXPORT_SYMBOL(omap_rfbi_write_command);

void omap_rfbi_read_data(void *buf, u32 len)
{
	rfbi_enable_clocks(1);
	switch (rfbi.parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		u8 *b = buf;
		for (; len; len--) {
			rfbi_write_reg(RFBI_READ, 0);
			*b++ = rfbi_read_reg(RFBI_READ);
		}
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		u16 *w = buf;
		BUG_ON(len & ~1);
		for (; len; len -= 2) {
			rfbi_write_reg(RFBI_READ, 0);
			*w++ = rfbi_read_reg(RFBI_READ);
		}
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();
	}
	rfbi_enable_clocks(0);
}
EXPORT_SYMBOL(omap_rfbi_read_data);

void omap_rfbi_write_data(const void *buf, u32 len)
{
	rfbi_enable_clocks(1);
	switch (rfbi.parallelmode) {
	case OMAP_DSS_RFBI_PARALLELMODE_8:
	{
		const u8 *b = buf;
		for (; len; len--)
			rfbi_write_reg(RFBI_PARAM, *b++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_16:
	{
		const u16 *w = buf;
		BUG_ON(len & 1);
		for (; len; len -= 2)
			rfbi_write_reg(RFBI_PARAM, *w++);
		break;
	}

	case OMAP_DSS_RFBI_PARALLELMODE_9:
	case OMAP_DSS_RFBI_PARALLELMODE_12:
	default:
		BUG();

	}
	rfbi_enable_clocks(0);
}
EXPORT_SYMBOL(omap_rfbi_write_data);

void omap_rfbi_write_pixels(const void __iomem *buf, int scr_width,
		u16 x, u16 y,
		u16 w, u16 h)
{
	int start_offset = scr_width * y + x;
	int horiz_offset = scr_width - w;
	int i;

	rfbi_enable_clocks(1);

	if (rfbi.datatype == OMAP_DSS_RFBI_DATATYPE_16 &&
	   rfbi.parallelmode == OMAP_DSS_RFBI_PARALLELMODE_8) {
		const u16 __iomem *pd = buf;
		pd += start_offset;

		for (; h; --h) {
			for (i = 0; i < w; ++i) {
				const u8 __iomem *b = (const u8 __iomem *)pd;
				rfbi_write_reg(RFBI_PARAM, __raw_readb(b+1));
				rfbi_write_reg(RFBI_PARAM, __raw_readb(b+0));
				++pd;
			}
			pd += horiz_offset;
		}
	} else if (rfbi.datatype == OMAP_DSS_RFBI_DATATYPE_24 &&
	   rfbi.parallelmode == OMAP_DSS_RFBI_PARALLELMODE_8) {
		const u32 __iomem *pd = buf;
		pd += start_offset;

		for (; h; --h) {
			for (i = 0; i < w; ++i) {
				const u8 __iomem *b = (const u8 __iomem *)pd;
				rfbi_write_reg(RFBI_PARAM, __raw_readb(b+2));
				rfbi_write_reg(RFBI_PARAM, __raw_readb(b+1));
				rfbi_write_reg(RFBI_PARAM, __raw_readb(b+0));
				++pd;
			}
			pd += horiz_offset;
		}
	} else if (rfbi.datatype == OMAP_DSS_RFBI_DATATYPE_16 &&
	   rfbi.parallelmode == OMAP_DSS_RFBI_PARALLELMODE_16) {
		const u16 __iomem *pd = buf;
		pd += start_offset;

		for (; h; --h) {
			for (i = 0; i < w; ++i) {
				rfbi_write_reg(RFBI_PARAM, __raw_readw(pd));
				++pd;
			}
			pd += horiz_offset;
		}
	} else {
		BUG();
	}

	rfbi_enable_clocks(0);
}
EXPORT_SYMBOL(omap_rfbi_write_pixels);

#ifdef MEASURE_PERF
static void perf_mark_setup(void)
{
	rfbi.perf_setup_time = ktime_get();
}

static void perf_mark_start(void)
{
	rfbi.perf_start_time = ktime_get();
}

static void perf_show(const char *name)
{
	ktime_t t, setup_time, trans_time;
	u32 total_bytes;
	u32 setup_us, trans_us, total_us;

	t = ktime_get();

	setup_time = ktime_sub(rfbi.perf_start_time, rfbi.perf_setup_time);
	setup_us = (u32)ktime_to_us(setup_time);
	if (setup_us == 0)
		setup_us = 1;

	trans_time = ktime_sub(t, rfbi.perf_start_time);
	trans_us = (u32)ktime_to_us(trans_time);
	if (trans_us == 0)
		trans_us = 1;

	total_us = setup_us + trans_us;

	total_bytes = rfbi.perf_bytes;

	DSSINFO("%s update %u us + %u us = %u us (%uHz), %u bytes, "
			"%u kbytes/sec\n",
			name,
			setup_us,
			trans_us,
			total_us,
			1000*1000 / total_us,
			total_bytes,
			total_bytes * 1000 / total_us);
}
#else
#define perf_mark_setup()
#define perf_mark_start()
#define perf_show(x)
#endif

void rfbi_transfer_area(u16 width, u16 height,
			     void (callback)(void *data), void *data)
{
	u32 l;

	/*BUG_ON(callback == 0);*/
	BUG_ON(rfbi.framedone_callback != NULL);

	DSSDBG("rfbi_transfer_area %dx%d\n", width, height);

	dispc_set_lcd_size(width, height);

	dispc_enable_lcd_out(1);

	rfbi.framedone_callback = callback;
	rfbi.framedone_callback_data = data;

	rfbi_enable_clocks(1);

	rfbi_write_reg(RFBI_PIXEL_CNT, width * height);

	l = rfbi_read_reg(RFBI_CONTROL);
	l = FLD_MOD(l, 1, 0, 0); /* enable */
	if (!rfbi.te_enabled)
		l = FLD_MOD(l, 1, 4, 4); /* ITE */

	perf_mark_start();

	rfbi_write_reg(RFBI_CONTROL, l);
}

static void framedone_callback(void *data, u32 mask)
{
	void (*callback)(void *data);

	DSSDBG("FRAMEDONE\n");

	perf_show("DISPC");

	REG_FLD_MOD(RFBI_CONTROL, 0, 0, 0);

	rfbi_enable_clocks(0);

	callback = rfbi.framedone_callback;
	rfbi.framedone_callback = NULL;

	/*callback(rfbi.framedone_callback_data);*/

	atomic_set(&rfbi.cmd_pending, 0);

	process_cmd_fifo();
}

#if 1 /* VERBOSE */
static void rfbi_print_timings(void)
{
	u32 l;
	u32 time;

	l = rfbi_read_reg(RFBI_CONFIG(0));
	time = 1000000000 / rfbi.l4_khz;
	if (l & (1 << 4))
		time *= 2;

	DSSDBG("Tick time %u ps\n", time);
	l = rfbi_read_reg(RFBI_ONOFF_TIME(0));
	DSSDBG("CSONTIME %d, CSOFFTIME %d, WEONTIME %d, WEOFFTIME %d, "
		"REONTIME %d, REOFFTIME %d\n",
		l & 0x0f, (l >> 4) & 0x3f, (l >> 10) & 0x0f, (l >> 14) & 0x3f,
		(l >> 20) & 0x0f, (l >> 24) & 0x3f);

	l = rfbi_read_reg(RFBI_CYCLE_TIME(0));
	DSSDBG("WECYCLETIME %d, RECYCLETIME %d, CSPULSEWIDTH %d, "
		"ACCESSTIME %d\n",
		(l & 0x3f), (l >> 6) & 0x3f, (l >> 12) & 0x3f,
		(l >> 22) & 0x3f);
}
#else
static void rfbi_print_timings(void) {}
#endif




static u32 extif_clk_period;

static inline unsigned long round_to_extif_ticks(unsigned long ps, int div)
{
	int bus_tick = extif_clk_period * div;
	return (ps + bus_tick - 1) / bus_tick * bus_tick;
}

static int calc_reg_timing(struct rfbi_timings *t, int div)
{
	t->clk_div = div;

	t->cs_on_time = round_to_extif_ticks(t->cs_on_time, div);

	t->we_on_time = round_to_extif_ticks(t->we_on_time, div);
	t->we_off_time = round_to_extif_ticks(t->we_off_time, div);
	t->we_cycle_time = round_to_extif_ticks(t->we_cycle_time, div);

	t->re_on_time = round_to_extif_ticks(t->re_on_time, div);
	t->re_off_time = round_to_extif_ticks(t->re_off_time, div);
	t->re_cycle_time = round_to_extif_ticks(t->re_cycle_time, div);

	t->access_time = round_to_extif_ticks(t->access_time, div);
	t->cs_off_time = round_to_extif_ticks(t->cs_off_time, div);
	t->cs_pulse_width = round_to_extif_ticks(t->cs_pulse_width, div);

	DSSDBG("[reg]cson %d csoff %d reon %d reoff %d\n",
	       t->cs_on_time, t->cs_off_time, t->re_on_time, t->re_off_time);
	DSSDBG("[reg]weon %d weoff %d recyc %d wecyc %d\n",
	       t->we_on_time, t->we_off_time, t->re_cycle_time,
	       t->we_cycle_time);
	DSSDBG("[reg]rdaccess %d cspulse %d\n",
	       t->access_time, t->cs_pulse_width);

	return rfbi_convert_timings(t);
}

static int calc_extif_timings(struct rfbi_timings *t)
{
	u32 max_clk_div;
	int div;

	rfbi_get_clk_info(&extif_clk_period, &max_clk_div);
	for (div = 1; div <= max_clk_div; div++) {
		if (calc_reg_timing(t, div) == 0)
			break;
	}

	if (div <= max_clk_div)
		return 0;

	DSSERR("can't setup timings\n");
	return -1;
}


void rfbi_set_timings(int rfbi_module, struct rfbi_timings *t)
{
	int r;

	if (!t->converted) {
		r = calc_extif_timings(t);
		if (r < 0)
			DSSERR("Failed to calc timings\n");
	}

	BUG_ON(!t->converted);

	rfbi_enable_clocks(1);
	rfbi_write_reg(RFBI_ONOFF_TIME(rfbi_module), t->tim[0]);
	rfbi_write_reg(RFBI_CYCLE_TIME(rfbi_module), t->tim[1]);

	/* TIMEGRANULARITY */
	REG_FLD_MOD(RFBI_CONFIG(rfbi_module),
		    (t->tim[2] ? 1 : 0), 4, 4);

	rfbi_print_timings();
	rfbi_enable_clocks(0);
}

static int ps_to_rfbi_ticks(int time, int div)
{
	unsigned long tick_ps;
	int ret;

	/* Calculate in picosecs to yield more exact results */
	tick_ps = 1000000000 / (rfbi.l4_khz) * div;

	ret = (time + tick_ps - 1) / tick_ps;

	return ret;
}

#ifdef OMAP_RFBI_RATE_LIMIT
unsigned long rfbi_get_max_tx_rate(void)
{
	unsigned long   l4_rate, dss1_rate;
	int             min_l4_ticks = 0;
	int             i;

	/* According to TI this can't be calculated so make the
	 * adjustments for a couple of known frequencies and warn for
	 * others.
	 */
	static const struct {
		unsigned long l4_clk;           /* HZ */
		unsigned long dss1_clk;         /* HZ */
		unsigned long min_l4_ticks;
	} ftab[] = {
		{ 55,   132,    7, },           /* 7.86 MPix/s */
		{ 110,  110,    12, },          /* 9.16 MPix/s */
		{ 110,  132,    10, },          /* 11   Mpix/s */
		{ 120,  120,    10, },          /* 12   Mpix/s */
		{ 133,  133,    10, },          /* 13.3 Mpix/s */
	};

	l4_rate = rfbi.l4_khz / 1000;
	dss1_rate = dss_clk_get_rate(DSS_CLK_FCK1) / 1000000;

	for (i = 0; i < ARRAY_SIZE(ftab); i++) {
		/* Use a window instead of an exact match, to account
		 * for different DPLL multiplier / divider pairs.
		 */
		if (abs(ftab[i].l4_clk - l4_rate) < 3 &&
		    abs(ftab[i].dss1_clk - dss1_rate) < 3) {
			min_l4_ticks = ftab[i].min_l4_ticks;
			break;
		}
	}
	if (i == ARRAY_SIZE(ftab)) {
		/* Can't be sure, return anyway the maximum not
		 * rate-limited. This might cause a problem only for the
		 * tearing synchronisation.
		 */
		DSSERR("can't determine maximum RFBI transfer rate\n");
		return rfbi.l4_khz * 1000;
	}
	return rfbi.l4_khz * 1000 / min_l4_ticks;
}
#else
int rfbi_get_max_tx_rate(void)
{
	return rfbi.l4_khz * 1000;
}
#endif

static void rfbi_get_clk_info(u32 *clk_period, u32 *max_clk_div)
{
	*clk_period = 1000000000 / rfbi.l4_khz;
	*max_clk_div = 2;
}

static int rfbi_convert_timings(struct rfbi_timings *t)
{
	u32 l;
	int reon, reoff, weon, weoff, cson, csoff, cs_pulse;
	int actim, recyc, wecyc;
	int div = t->clk_div;

	if (div <= 0 || div > 2)
		return -1;

	/* Make sure that after conversion it still holds that:
	 * weoff > weon, reoff > reon, recyc >= reoff, wecyc >= weoff,
	 * csoff > cson, csoff >= max(weoff, reoff), actim > reon
	 */
	weon = ps_to_rfbi_ticks(t->we_on_time, div);
	weoff = ps_to_rfbi_ticks(t->we_off_time, div);
	if (weoff <= weon)
		weoff = weon + 1;
	if (weon > 0x0f)
		return -1;
	if (weoff > 0x3f)
		return -1;

	reon = ps_to_rfbi_ticks(t->re_on_time, div);
	reoff = ps_to_rfbi_ticks(t->re_off_time, div);
	if (reoff <= reon)
		reoff = reon + 1;
	if (reon > 0x0f)
		return -1;
	if (reoff > 0x3f)
		return -1;

	cson = ps_to_rfbi_ticks(t->cs_on_time, div);
	csoff = ps_to_rfbi_ticks(t->cs_off_time, div);
	if (csoff <= cson)
		csoff = cson + 1;
	if (csoff < max(weoff, reoff))
		csoff = max(weoff, reoff);
	if (cson > 0x0f)
		return -1;
	if (csoff > 0x3f)
		return -1;

	l =  cson;
	l |= csoff << 4;
	l |= weon  << 10;
	l |= weoff << 14;
	l |= reon  << 20;
	l |= reoff << 24;

	t->tim[0] = l;

	actim = ps_to_rfbi_ticks(t->access_time, div);
	if (actim <= reon)
		actim = reon + 1;
	if (actim > 0x3f)
		return -1;

	wecyc = ps_to_rfbi_ticks(t->we_cycle_time, div);
	if (wecyc < weoff)
		wecyc = weoff;
	if (wecyc > 0x3f)
		return -1;

	recyc = ps_to_rfbi_ticks(t->re_cycle_time, div);
	if (recyc < reoff)
		recyc = reoff;
	if (recyc > 0x3f)
		return -1;

	cs_pulse = ps_to_rfbi_ticks(t->cs_pulse_width, div);
	if (cs_pulse > 0x3f)
		return -1;

	l =  wecyc;
	l |= recyc    << 6;
	l |= cs_pulse << 12;
	l |= actim    << 22;

	t->tim[1] = l;

	t->tim[2] = div - 1;

	t->converted = 1;

	return 0;
}

/* xxx FIX module selection missing */
int omap_rfbi_setup_te(enum omap_rfbi_te_mode mode,
			     unsigned hs_pulse_time, unsigned vs_pulse_time,
			     int hs_pol_inv, int vs_pol_inv, int extif_div)
{
	int hs, vs;
	int min;
	u32 l;

	hs = ps_to_rfbi_ticks(hs_pulse_time, 1);
	vs = ps_to_rfbi_ticks(vs_pulse_time, 1);
	if (hs < 2)
		return -EDOM;
	if (mode == OMAP_DSS_RFBI_TE_MODE_2)
		min = 2;
	else /* OMAP_DSS_RFBI_TE_MODE_1 */
		min = 4;
	if (vs < min)
		return -EDOM;
	if (vs == hs)
		return -EINVAL;
	rfbi.te_mode = mode;
	DSSDBG("setup_te: mode %d hs %d vs %d hs_inv %d vs_inv %d\n",
		mode, hs, vs, hs_pol_inv, vs_pol_inv);

	rfbi_enable_clocks(1);
	rfbi_write_reg(RFBI_HSYNC_WIDTH, hs);
	rfbi_write_reg(RFBI_VSYNC_WIDTH, vs);

	l = rfbi_read_reg(RFBI_CONFIG(0));
	if (hs_pol_inv)
		l &= ~(1 << 21);
	else
		l |= 1 << 21;
	if (vs_pol_inv)
		l &= ~(1 << 20);
	else
		l |= 1 << 20;
	rfbi_enable_clocks(0);

	return 0;
}
EXPORT_SYMBOL(omap_rfbi_setup_te);

/* xxx FIX module selection missing */
int omap_rfbi_enable_te(bool enable, unsigned line)
{
	u32 l;

	DSSDBG("te %d line %d mode %d\n", enable, line, rfbi.te_mode);
	if (line > (1 << 11) - 1)
		return -EINVAL;

	rfbi_enable_clocks(1);
	l = rfbi_read_reg(RFBI_CONFIG(0));
	l &= ~(0x3 << 2);
	if (enable) {
		rfbi.te_enabled = 1;
		l |= rfbi.te_mode << 2;
	} else
		rfbi.te_enabled = 0;
	rfbi_write_reg(RFBI_CONFIG(0), l);
	rfbi_write_reg(RFBI_LINE_NUMBER, line);
	rfbi_enable_clocks(0);

	return 0;
}
EXPORT_SYMBOL(omap_rfbi_enable_te);

#if 0
static void rfbi_enable_config(int enable1, int enable2)
{
	u32 l;
	int cs = 0;

	if (enable1)
		cs |= 1<<0;
	if (enable2)
		cs |= 1<<1;

	rfbi_enable_clocks(1);

	l = rfbi_read_reg(RFBI_CONTROL);

	l = FLD_MOD(l, cs, 3, 2);
	l = FLD_MOD(l, 0, 1, 1);

	rfbi_write_reg(RFBI_CONTROL, l);


	l = rfbi_read_reg(RFBI_CONFIG(0));
	l = FLD_MOD(l, 0, 3, 2); /* TRIGGERMODE: ITE */
	/*l |= FLD_VAL(2, 8, 7); */ /* L4FORMAT, 2pix/L4 */
	/*l |= FLD_VAL(0, 8, 7); */ /* L4FORMAT, 1pix/L4 */

	l = FLD_MOD(l, 0, 16, 16); /* A0POLARITY */
	l = FLD_MOD(l, 1, 20, 20); /* TE_VSYNC_POLARITY */
	l = FLD_MOD(l, 1, 21, 21); /* HSYNCPOLARITY */

	l = FLD_MOD(l, OMAP_DSS_RFBI_PARALLELMODE_8, 1, 0);
	rfbi_write_reg(RFBI_CONFIG(0), l);

	rfbi_enable_clocks(0);
}
#endif

int rfbi_configure(int rfbi_module, int bpp, int lines)
{
	u32 l;
	int cycle1 = 0, cycle2 = 0, cycle3 = 0;
	enum omap_rfbi_cycleformat cycleformat;
	enum omap_rfbi_datatype datatype;
	enum omap_rfbi_parallelmode parallelmode;

	switch (bpp) {
	case 12:
		datatype = OMAP_DSS_RFBI_DATATYPE_12;
		break;
	case 16:
		datatype = OMAP_DSS_RFBI_DATATYPE_16;
		break;
	case 18:
		datatype = OMAP_DSS_RFBI_DATATYPE_18;
		break;
	case 24:
		datatype = OMAP_DSS_RFBI_DATATYPE_24;
		break;
	default:
		BUG();
		return 1;
	}
	rfbi.datatype = datatype;

	switch (lines) {
	case 8:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_8;
		break;
	case 9:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_9;
		break;
	case 12:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_12;
		break;
	case 16:
		parallelmode = OMAP_DSS_RFBI_PARALLELMODE_16;
		break;
	default:
		BUG();
		return 1;
	}
	rfbi.parallelmode = parallelmode;

	if ((bpp % lines) == 0) {
		switch (bpp / lines) {
		case 1:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_1_1;
			break;
		case 2:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_2_1;
			break;
		case 3:
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_3_1;
			break;
		default:
			BUG();
			return 1;
		}
	} else if ((2 * bpp % lines) == 0) {
		if ((2 * bpp / lines) == 3)
			cycleformat = OMAP_DSS_RFBI_CYCLEFORMAT_3_2;
		else {
			BUG();
			return 1;
		}
	} else {
		BUG();
		return 1;
	}

	switch (cycleformat) {
	case OMAP_DSS_RFBI_CYCLEFORMAT_1_1:
		cycle1 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_2_1:
		cycle1 = lines;
		cycle2 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_3_1:
		cycle1 = lines;
		cycle2 = lines;
		cycle3 = lines;
		break;

	case OMAP_DSS_RFBI_CYCLEFORMAT_3_2:
		cycle1 = lines;
		cycle2 = (lines / 2) | ((lines / 2) << 16);
		cycle3 = (lines << 16);
		break;
	}

	rfbi_enable_clocks(1);

	REG_FLD_MOD(RFBI_CONTROL, 0, 3, 2); /* clear CS */

	l = 0;
	l |= FLD_VAL(parallelmode, 1, 0);
	l |= FLD_VAL(0, 3, 2);		/* TRIGGERMODE: ITE */
	l |= FLD_VAL(0, 4, 4);		/* TIMEGRANULARITY */
	l |= FLD_VAL(datatype, 6, 5);
	/* l |= FLD_VAL(2, 8, 7); */	/* L4FORMAT, 2pix/L4 */
	l |= FLD_VAL(0, 8, 7);	/* L4FORMAT, 1pix/L4 */
	l |= FLD_VAL(cycleformat, 10, 9);
	l |= FLD_VAL(0, 12, 11);	/* UNUSEDBITS */
	l |= FLD_VAL(0, 16, 16);	/* A0POLARITY */
	l |= FLD_VAL(0, 17, 17);	/* REPOLARITY */
	l |= FLD_VAL(0, 18, 18);	/* WEPOLARITY */
	l |= FLD_VAL(0, 19, 19);	/* CSPOLARITY */
	l |= FLD_VAL(1, 20, 20);	/* TE_VSYNC_POLARITY */
	l |= FLD_VAL(1, 21, 21);	/* HSYNCPOLARITY */
	rfbi_write_reg(RFBI_CONFIG(rfbi_module), l);

	rfbi_write_reg(RFBI_DATA_CYCLE1(rfbi_module), cycle1);
	rfbi_write_reg(RFBI_DATA_CYCLE2(rfbi_module), cycle2);
	rfbi_write_reg(RFBI_DATA_CYCLE3(rfbi_module), cycle3);


	l = rfbi_read_reg(RFBI_CONTROL);
	l = FLD_MOD(l, rfbi_module+1, 3, 2); /* Select CSx */
	l = FLD_MOD(l, 0, 1, 1); /* clear bypass */
	rfbi_write_reg(RFBI_CONTROL, l);


	DSSDBG("RFBI config: bpp %d, lines %d, cycles: 0x%x 0x%x 0x%x\n",
	       bpp, lines, cycle1, cycle2, cycle3);

	rfbi_enable_clocks(0);

	return 0;
}
EXPORT_SYMBOL(rfbi_configure);

static int rfbi_find_display(struct omap_display *disp)
{
	if (disp == rfbi.display[0])
		return 0;

	if (disp == rfbi.display[1])
		return 1;

	BUG();
	return -1;
}


static void signal_fifo_waiters(void)
{
	if (atomic_read(&rfbi.cmd_fifo_full) > 0) {
		/* DSSDBG("SIGNALING: Fifo not full for waiter!\n"); */
		complete(&rfbi.cmd_done);
		atomic_dec(&rfbi.cmd_fifo_full);
	}
}

/* returns 1 for async op, and 0 for sync op */
static int do_update(struct omap_display *display, struct update_region *upd)
{
	u16 x = upd->x;
	u16 y = upd->y;
	u16 w = upd->w;
	u16 h = upd->h;

	perf_mark_setup();

	if (display->manager->caps & OMAP_DSS_OVL_MGR_CAP_DISPC) {
		/*display->ctrl->enable_te(display, 1); */
		dispc_setup_partial_planes(display, &x, &y, &w, &h);
	}

#ifdef MEASURE_PERF
	rfbi.perf_bytes = w * h * 2; /* XXX always 16bit */
#endif

	display->ctrl->setup_update(display, x, y, w, h);

	if (display->manager->caps & OMAP_DSS_OVL_MGR_CAP_DISPC) {
		rfbi_transfer_area(w, h, NULL, NULL);
		return 1;
	} else {
		struct omap_overlay *ovl;
		void __iomem *addr;
		int scr_width;

		ovl = display->manager->overlays[0];
		scr_width = ovl->info.screen_width;
		addr = ovl->info.vaddr;

		omap_rfbi_write_pixels(addr, scr_width, x, y, w, h);

		perf_show("L4");

		return 0;
	}
}

static void process_cmd_fifo(void)
{
	int len;
	struct update_param p;
	struct omap_display *display;
	unsigned long flags;

	if (atomic_inc_return(&rfbi.cmd_pending) != 1)
		return;

	while (true) {
		spin_lock_irqsave(rfbi.cmd_fifo->lock, flags);

		len = __kfifo_get(rfbi.cmd_fifo, (unsigned char *)&p,
				  sizeof(struct update_param));
		if (len == 0) {
			DSSDBG("nothing more in fifo\n");
			atomic_set(&rfbi.cmd_pending, 0);
			spin_unlock_irqrestore(rfbi.cmd_fifo->lock, flags);
			break;
		}

		/* DSSDBG("fifo full %d\n", rfbi.cmd_fifo_full.counter);*/

		spin_unlock_irqrestore(rfbi.cmd_fifo->lock, flags);

		BUG_ON(len != sizeof(struct update_param));
		BUG_ON(p.rfbi_module > 1);

		display = rfbi.display[p.rfbi_module];

		if (p.cmd == RFBI_CMD_UPDATE) {
			if (do_update(display, &p.par.r))
				break; /* async op */
		} else if (p.cmd == RFBI_CMD_SYNC) {
			DSSDBG("Signaling SYNC done!\n");
			complete(p.par.sync);
		} else
			BUG();
	}

	signal_fifo_waiters();
}

static void rfbi_push_cmd(struct update_param *p)
{
	int ret;

	while (1) {
		unsigned long flags;
		int available;

		spin_lock_irqsave(rfbi.cmd_fifo->lock, flags);
		available = RFBI_CMD_FIFO_LEN_BYTES -
			__kfifo_len(rfbi.cmd_fifo);

/*		DSSDBG("%d bytes left in fifo\n", available); */
		if (available < sizeof(struct update_param)) {
			DSSDBG("Going to wait because FIFO FULL..\n");
			spin_unlock_irqrestore(rfbi.cmd_fifo->lock, flags);
			atomic_inc(&rfbi.cmd_fifo_full);
			wait_for_completion(&rfbi.cmd_done);
			/*DSSDBG("Woke up because fifo not full anymore\n");*/
			continue;
		}

		ret = __kfifo_put(rfbi.cmd_fifo, (unsigned char *)p,
				  sizeof(struct update_param));
/*		DSSDBG("pushed %d bytes\n", ret);*/

		spin_unlock_irqrestore(rfbi.cmd_fifo->lock, flags);

		BUG_ON(ret != sizeof(struct update_param));

		break;
	}
}

static void rfbi_push_update(int rfbi_module, int x, int y, int w, int h)
{
	struct update_param p;

	p.rfbi_module = rfbi_module;
	p.cmd = RFBI_CMD_UPDATE;

	p.par.r.x = x;
	p.par.r.y = y;
	p.par.r.w = w;
	p.par.r.h = h;

	DSSDBG("RFBI pushed %d,%d %dx%d\n", x, y, w, h);

	rfbi_push_cmd(&p);

	process_cmd_fifo();
}

static void rfbi_push_sync(int rfbi_module, struct completion *sync_comp)
{
	struct update_param p;

	p.rfbi_module = rfbi_module;
	p.cmd = RFBI_CMD_SYNC;
	p.par.sync = sync_comp;

	rfbi_push_cmd(&p);

	DSSDBG("RFBI sync pushed to cmd fifo\n");

	process_cmd_fifo();
}

void rfbi_dump_regs(struct seq_file *s)
{
#define DUMPREG(r) seq_printf(s, "%-35s %08x\n", #r, rfbi_read_reg(r))

	dss_clk_enable(DSS_CLK_ICK | DSS_CLK_FCK1);

	DUMPREG(RFBI_REVISION);
	DUMPREG(RFBI_SYSCONFIG);
	DUMPREG(RFBI_SYSSTATUS);
	DUMPREG(RFBI_CONTROL);
	DUMPREG(RFBI_PIXEL_CNT);
	DUMPREG(RFBI_LINE_NUMBER);
	DUMPREG(RFBI_CMD);
	DUMPREG(RFBI_PARAM);
	DUMPREG(RFBI_DATA);
	DUMPREG(RFBI_READ);
	DUMPREG(RFBI_STATUS);

	DUMPREG(RFBI_CONFIG(0));
	DUMPREG(RFBI_ONOFF_TIME(0));
	DUMPREG(RFBI_CYCLE_TIME(0));
	DUMPREG(RFBI_DATA_CYCLE1(0));
	DUMPREG(RFBI_DATA_CYCLE2(0));
	DUMPREG(RFBI_DATA_CYCLE3(0));

	DUMPREG(RFBI_CONFIG(1));
	DUMPREG(RFBI_ONOFF_TIME(1));
	DUMPREG(RFBI_CYCLE_TIME(1));
	DUMPREG(RFBI_DATA_CYCLE1(1));
	DUMPREG(RFBI_DATA_CYCLE2(1));
	DUMPREG(RFBI_DATA_CYCLE3(1));

	DUMPREG(RFBI_VSYNC_WIDTH);
	DUMPREG(RFBI_HSYNC_WIDTH);

	dss_clk_disable(DSS_CLK_ICK | DSS_CLK_FCK1);
#undef DUMPREG
}

int rfbi_init(void)
{
	u32 rev;
	u32 l;

	spin_lock_init(&rfbi.cmd_lock);
	rfbi.cmd_fifo = kfifo_alloc(RFBI_CMD_FIFO_LEN_BYTES, GFP_KERNEL,
				    &rfbi.cmd_lock);
	if (IS_ERR(rfbi.cmd_fifo))
		return -ENOMEM;

	init_completion(&rfbi.cmd_done);
	atomic_set(&rfbi.cmd_fifo_full, 0);
	atomic_set(&rfbi.cmd_pending, 0);

	rfbi.base = ioremap(RFBI_BASE, SZ_256);
	if (!rfbi.base) {
		DSSERR("can't ioremap RFBI\n");
		return -ENOMEM;
	}

	rfbi_enable_clocks(1);

	msleep(10);

	rfbi.l4_khz = dss_clk_get_rate(DSS_CLK_ICK) / 1000;

	/* Enable autoidle and smart-idle */
	l = rfbi_read_reg(RFBI_SYSCONFIG);
	l |= (1 << 0) | (2 << 3);
	rfbi_write_reg(RFBI_SYSCONFIG, l);

	rev = rfbi_read_reg(RFBI_REVISION);
	printk(KERN_INFO "OMAP RFBI rev %d.%d\n",
	       FLD_GET(rev, 7, 4), FLD_GET(rev, 3, 0));

	rfbi_enable_clocks(0);

	return 0;
}

void rfbi_exit(void)
{
	DSSDBG("rfbi_exit\n");

	kfifo_free(rfbi.cmd_fifo);

	iounmap(rfbi.base);
}

/* struct omap_display support */
static int rfbi_display_update(struct omap_display *display,
			u16 x, u16 y, u16 w, u16 h)
{
	int rfbi_module;

	if (w == 0 || h == 0)
		return 0;

	rfbi_module = rfbi_find_display(display);

	rfbi_push_update(rfbi_module, x, y, w, h);

	return 0;
}

static int rfbi_display_sync(struct omap_display *display)
{
	struct completion sync_comp;
	int rfbi_module;

	rfbi_module = rfbi_find_display(display);

	init_completion(&sync_comp);
	rfbi_push_sync(rfbi_module, &sync_comp);
	DSSDBG("Waiting for SYNC to happen...\n");
	wait_for_completion(&sync_comp);
	DSSDBG("Released from SYNC\n");
	return 0;
}

static int rfbi_display_enable_te(struct omap_display *display, bool enable)
{
	display->ctrl->enable_te(display, enable);
	return 0;
}

static int rfbi_display_enable(struct omap_display *display)
{
	int r;

	BUG_ON(display->panel == NULL || display->ctrl == NULL);

	r = omap_dispc_register_isr(framedone_callback, NULL,
			DISPC_IRQ_FRAMEDONE);
	if (r) {
		DSSERR("can't get FRAMEDONE irq\n");
		return r;
	}

	dispc_set_lcd_display_type(OMAP_DSS_LCD_DISPLAY_TFT);

	dispc_set_parallel_interface_mode(OMAP_DSS_PARALLELMODE_RFBI);

	dispc_set_tft_data_lines(display->ctrl->pixel_size);

	rfbi_configure(display->hw_config.u.rfbi.channel,
			       display->ctrl->pixel_size,
			       display->hw_config.u.rfbi.data_lines);

	rfbi_set_timings(display->hw_config.u.rfbi.channel,
			 &display->ctrl->timings);


	if (display->ctrl && display->ctrl->enable) {
		r = display->ctrl->enable(display);
		if (r)
			goto err;
	}

	if (display->panel && display->panel->enable) {
		r = display->panel->enable(display);
		if (r)
			goto err;
	}

	return 0;
err:
	return -ENODEV;
}

static void rfbi_display_disable(struct omap_display *display)
{
	display->ctrl->disable(display);
	omap_dispc_unregister_isr(framedone_callback, NULL,
			DISPC_IRQ_FRAMEDONE);
}

int rfbi_init_display(struct omap_display *display)
{
	display->enable = rfbi_display_enable;
	display->disable = rfbi_display_disable;
	display->update = rfbi_display_update;
	display->sync = rfbi_display_sync;
	display->enable_te = rfbi_display_enable_te;

	rfbi.display[display->hw_config.u.rfbi.channel] = display;

	display->caps = OMAP_DSS_DISPLAY_CAP_MANUAL_UPDATE;

	return 0;
}
