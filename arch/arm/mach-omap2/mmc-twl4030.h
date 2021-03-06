/*
 * MMC definitions for OMAP2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

struct twl4030_hsmmc_info {
	u8	mmc;		/* controller 1/2/3 */
	u8	wires;		/* 1/4/8 wires */
	bool	transceiver;	/* MMC-2 option */
	bool	ext_clock;	/* use external pin for input clock */
	u8      vmmc_dev_grp;   /* override default regulator */
	bool	vsim_18v;	/* MMC-2 option */
	bool	cover_only;	/* No card detect - just cover switch */
	bool	power_saving;	/* Try to sleep or power off when possible */
	unsigned long caps;	/* MMC host capabilities */
	int	gpio_cd;	/* or -EINVAL */
	int	gpio_wp;	/* or -EINVAL */
	char	*name;		/* or NULL for default */
};

#if	defined(CONFIG_TWL4030_CORE) && \
	(defined(CONFIG_MMC_OMAP) || defined(CONFIG_MMC_OMAP_MODULE) || \
	 defined(CONFIG_MMC_OMAP_HS) || defined(CONFIG_MMC_OMAP_HS_MODULE))

void twl4030_mmc_init(struct twl4030_hsmmc_info *);

#else

static inline void twl4030_mmc_init(struct twl4030_hsmmc_info *info)
{
}

#endif
