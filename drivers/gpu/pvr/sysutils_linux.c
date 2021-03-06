/**********************************************************************
 *
 * Copyright(c) 2008 Imagination Technologies Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful but, except
 * as otherwise stated in writing, without any warranty; without even the
 * implied warranty of merchantability or fitness for a particular purpose.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK
 *
 ******************************************************************************/

#include <linux/version.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/hardirq.h>
#include <mach/omap-pm.h>
#include <linux/bug.h>
#include <mach/clock.h>

#include "sgxdefs.h"
#include "services_headers.h"
#include "sysinfo.h"
#include "sgxapi_km.h"
#include "sysconfig.h"
#include "sgxinfokm.h"
#include "syslocal.h"
#include "env_data.h"

#define	HZ_TO_MHZ(m) ((m) / 1000000)

static inline unsigned long scale_by_rate(unsigned long val,
					  unsigned long rate1,
					  unsigned long rate2)
{
	if (rate1 >= rate2)
		return val * (rate1 / rate2);

	return val / (rate2 / rate1);
}

static inline unsigned long scale_prop_to_SGX_clock(unsigned long val,
						    unsigned long rate)
{
	return scale_by_rate(val, rate, SYS_SGX_CLOCK_SPEED);
}

static inline unsigned long scale_inv_prop_to_SGX_clock(unsigned long val,
							unsigned long rate)
{
	return scale_by_rate(val, SYS_SGX_CLOCK_SPEED, rate);
}

void SysGetSGXTimingInformation(struct SGX_TIMING_INFORMATION *psTimingInfo)
{
	unsigned long rate;

	PVR_ASSERT(gpsSysSpecificData->bSGXClocksEnabled);

	rate = clk_get_rate(gpsSysSpecificData->psSGX_FCK);
	PVR_ASSERT(rate != 0);
	psTimingInfo->ui32CoreClockSpeed = rate;
	psTimingInfo->ui32HWRecoveryFreq =
	    scale_prop_to_SGX_clock(SYS_SGX_HWRECOVERY_TIMEOUT_FREQ, rate);
	psTimingInfo->ui32uKernelFreq =
	    scale_prop_to_SGX_clock(SYS_SGX_PDS_TIMER_FREQ, rate);
	psTimingInfo->ui32ActivePowManLatencyms =
	    SYS_SGX_ACTIVE_POWER_LATENCY_MS;
}


static int vdd2_post_func(struct notifier_block *n, unsigned long event,
			  void *ptr)
{
	PVR_UNREFERENCED_PARAMETER(n);
	PVR_UNREFERENCED_PARAMETER(event);
	PVR_UNREFERENCED_PARAMETER(ptr);

	if (gpsSysSpecificData->bSGXClocksEnabled &&
	    gpsSysSpecificData->bSGXInitComplete) {
#if defined(DEBUG)
		unsigned long rate;

		rate = clk_get_rate(gpsSysSpecificData->psSGX_FCK);

		PVR_ASSERT(rate != 0);

		PVR_TRACE("%s: SGX clock rate: %dMHz", __func__,
			   HZ_TO_MHZ(rate));
#endif
		PVRSRVDevicePostClockSpeedChange(gpsSysSpecificData->
						 psSGXDevNode->sDevId.
						 ui32DeviceIndex, IMG_TRUE,
						 NULL);
	}
	return 0;
}

static int vdd2_pre_func(struct notifier_block *n, unsigned long event,
			 void *ptr)
{
	PVR_UNREFERENCED_PARAMETER(n);
	PVR_UNREFERENCED_PARAMETER(event);
	PVR_UNREFERENCED_PARAMETER(ptr);

	if (gpsSysSpecificData->bSGXClocksEnabled
	    && gpsSysSpecificData->bSGXInitComplete) {
		BUG_ON(gpsSysData->eCurrentPowerState > PVRSRV_POWER_STATE_D1);
		PVRSRVDevicePreClockSpeedChange(gpsSysSpecificData->
						psSGXDevNode->sDevId.
						ui32DeviceIndex, IMG_TRUE,
						NULL);
	}

	return 0;
}

static int vdd2_pre_post_func(struct notifier_block *n, unsigned long event,
			      void *ptr)
{
	struct clk_notifier_data *cnd;

	PVR_UNREFERENCED_PARAMETER(n);

	cnd = (struct clk_notifier_data *)ptr;

	PVR_TRACE("vdd2_pre_post_func: old clock rate = %lu", cnd->old_rate);
	PVR_TRACE("vdd2_pre_post_func: new clock rate = %lu", cnd->new_rate);

	if (CLK_PRE_RATE_CHANGE == event) {
		PVRSRVDvfsLock();
		PVR_TRACE("vdd2_pre_post_func: CLK_PRE_RATE_CHANGE event");
		vdd2_pre_func(n, event, ptr);
	} else if (CLK_POST_RATE_CHANGE == event) {
		PVR_TRACE("vdd2_pre_post_func: CLK_POST_RATE_CHANGE event");
		vdd2_post_func(n, event, ptr);
		PVRSRVDvfsUnlock();
	} else if (CLK_ABORT_RATE_CHANGE == event) {
		PVR_TRACE("vdd2_pre_post_func: CLK_ABORT_RATE_CHANGE event");
		PVRSRVDvfsUnlock();
	} else {
		printk(KERN_ERR "vdd2_pre_post_func: unexpected event (%lu)\n",
			event);
		PVR_DPF(PVR_DBG_ERROR,
			 "vdd2_pre_post_func: unexpected event (%lu)", event);
	}
	PVR_TRACE("vdd2_pre_post_func end.");
	return 0;
}

static struct notifier_block vdd2_pre_post = {
	vdd2_pre_post_func,
	NULL
};

static void RegisterConstraintNotifications(
					struct SYS_SPECIFIC_DATA *psSysSpecData)
{
	PVR_TRACE("Registering constraint notifications");

	clk_notifier_register(psSysSpecData->psSGX_FCK, &vdd2_pre_post);
	PVR_TRACE("VDD2 constraint notifications registered");
}

static void UnRegisterConstraintNotifications(
					struct SYS_SPECIFIC_DATA *psSysSpecData)
{
	PVR_TRACE("Unregistering constraint notifications");

	clk_notifier_unregister(psSysSpecData->psSGX_FCK, &vdd2_pre_post);
}

static struct device sgx_dev;
static int sgx_clock_enabled;

/* return value: current sgx load
 * 0 - not busy
 * 100 - busy
 */
static unsigned int sgx_current_load(void)
{
	enum PVRSRV_ERROR eError;
	struct SYS_DATA *psSysData;
	struct SYS_SPECIFIC_DATA *psSysSpecData;
	struct PVRSRV_DEVICE_NODE *psDeviceNode;
	static unsigned int kicks_prev;
	static long time_prev;

	eError = SysAcquireData(&psSysData);
	if (eError != PVRSRV_OK)
		return 0;
	psSysSpecData = (struct SYS_SPECIFIC_DATA *)
						psSysData->pvSysSpecificData;
	if ((!psSysSpecData) || (!psSysSpecData->bSGXClocksEnabled))
		return 0;
	psDeviceNode = psSysData->psDeviceNodeList;
	while (psDeviceNode) {
		if ((psDeviceNode->sDevId.eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		    && (psDeviceNode->pvDevice)) {
			struct PVRSRV_SGXDEV_INFO *psDevInfo =
			    (struct PVRSRV_SGXDEV_INFO *)psDeviceNode->pvDevice;
			unsigned int kicks = psDevInfo->ui32KickTACounter;
			unsigned int load;
			long time_elapsed;

			time_elapsed = jiffies - time_prev;
			if (likely(time_elapsed))
				load =
				    1000 * (kicks - kicks_prev) / time_elapsed;
			else
				load = 0;
			kicks_prev = kicks;
			time_prev += time_elapsed;
			/*
			 * if the period between calls to this function was
			 * too long then load stats are invalid.
			 */
			if (time_elapsed > 5 * HZ)
				return 0;
			/*pr_err("SGX load %u\n", load); */

			/*
			 * 'load' shows how many times sgx was kicked per
			 * 1000 jiffies 150 is arbitrarily chosen threshold.
			 * If the number of kicks is below threshold then sgx
			 * is doing some small jobs and we can keep the clocki
			 * freq low.
			 */
			if (load < 150)
				return 0;
			else
				return 100;
		}
		psDeviceNode = psDeviceNode->psNext;
	}
	return 0;
}

static void sgx_lock_perf(struct work_struct *work)
{
	int vdd1, vdd2;
	static int bHigh;
	int high;
	unsigned int load;
	struct delayed_work *d_work =
			container_of(work, struct delayed_work, work);
	struct ENV_DATA *psEnvData =
			container_of(d_work, struct ENV_DATA, sPerfWork);

	load = sgx_current_load();
	if (load) {
		vdd1 = 500000000;
		vdd2 = 400000;
		high = 1;
	} else {
		vdd1 = 0;
		vdd2 = 0;
		high = 0;
	}
	if (high != bHigh) {
		omap_pm_set_min_bus_tput(&sgx_dev, OCP_INITIATOR_AGENT, vdd2);
		omap_pm_set_min_mpu_freq(&sgx_dev, vdd1);
		bHigh = high;
	}
	if (sgx_clock_enabled || load)
		queue_delayed_work(psEnvData->psPerfWorkqueue,
				   &psEnvData->sPerfWork, HZ / 5);
}

static void sgx_need_perf(struct SYS_DATA *psSysData, int ena)
{
	struct ENV_DATA *psEnvData = (struct ENV_DATA *)
					psSysData->pvEnvSpecificData;

	sgx_clock_enabled = ena;
	cancel_delayed_work(&psEnvData->sPerfWork);
	queue_delayed_work(psEnvData->psPerfWorkqueue, &psEnvData->sPerfWork,
			   0);
}

enum PVRSRV_ERROR OSInitPerf(void *pvSysData)
{
	struct SYS_DATA *psSysData = (struct SYS_DATA *)pvSysData;
	struct ENV_DATA *psEnvData = (struct ENV_DATA *)
						psSysData->pvEnvSpecificData;

	if (psEnvData->psPerfWorkqueue) {
		PVR_DPF(PVR_DBG_ERROR, "OSInitPerf: already inited");
		return PVRSRV_ERROR_GENERIC;
	}

	PVR_TRACE("Initing DVFS %x", pvSysData);

	psEnvData->psPerfWorkqueue = create_singlethread_workqueue("sgx_perf");
	INIT_DELAYED_WORK(&psEnvData->sPerfWork, sgx_lock_perf);

	return PVRSRV_OK;
}

enum PVRSRV_ERROR OSCleanupPerf(void *pvSysData)
{
	struct SYS_DATA *psSysData = (struct SYS_DATA *)pvSysData;
	struct ENV_DATA *psEnvData = (struct ENV_DATA *)
						psSysData->pvEnvSpecificData;

	if (!psEnvData->psPerfWorkqueue) {
		PVR_DPF(PVR_DBG_ERROR, "OSCleanupPerf: not inited");
		return PVRSRV_ERROR_GENERIC;
	}

	PVR_TRACE("Cleaning up DVFS");

	flush_workqueue(psEnvData->psPerfWorkqueue);
	destroy_workqueue(psEnvData->psPerfWorkqueue);

	return PVRSRV_OK;
}

enum PVRSRV_ERROR EnableSGXClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData =
	    (struct SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;
#if defined(DEBUG)
	unsigned long rate;
#endif
	int res;

	if (psSysSpecData->bSGXClocksEnabled)
		return PVRSRV_OK;

	PVR_TRACE("EnableSGXClocks: Enabling SGX Clocks");

#if defined(DEBUG)
	rate = clk_get_rate(psSysSpecData->psMPU_CK);
	PVR_TRACE("CPU Clock is %dMhz", HZ_TO_MHZ(rate));
#endif

	res = clk_enable(psSysSpecData->psSGX_FCK);
	if (res < 0) {
		PVR_DPF(PVR_DBG_ERROR, "EnableSGXClocks: "
			"Couldn't enable SGX functional clock (%d)", res);
		return PVRSRV_ERROR_GENERIC;
	}

	res = clk_enable(psSysSpecData->psSGX_ICK);
	if (res < 0) {
		PVR_DPF(PVR_DBG_ERROR, "EnableSGXClocks: "
			"Couldn't enable SGX interface clock (%d)", res);

		clk_disable(psSysSpecData->psSGX_FCK);
		return PVRSRV_ERROR_GENERIC;
	}

	psSysSpecData->bSGXClocksEnabled = IMG_TRUE;
	sgx_need_perf(psSysData, 1);
	return PVRSRV_OK;
}

void DisableSGXClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData =
	    (struct SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;

	if (!psSysSpecData->bSGXClocksEnabled)
		return;

	PVR_TRACE("DisableSGXClocks: Disabling SGX Clocks");

	if (psSysSpecData->psSGX_ICK)
		clk_disable(psSysSpecData->psSGX_ICK);

	if (psSysSpecData->psSGX_FCK)
		clk_disable(psSysSpecData->psSGX_FCK);

	psSysSpecData->bSGXClocksEnabled = IMG_FALSE;
	sgx_need_perf(psSysData, 0);
}

static enum PVRSRV_ERROR InitSgxClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;
	struct clk *psCLK;
	struct clk *core_ck = NULL;

	psCLK = clk_get(NULL, "sgx_fck");
	if (IS_ERR(psCLK))
		goto err0;
	psSysSpecData->psSGX_FCK = psCLK;

	psCLK = clk_get(NULL, "sgx_ick");
	if (IS_ERR(psCLK))
		goto err1;
	psSysSpecData->psSGX_ICK = psCLK;

	core_ck = clk_get(NULL, "core_ck");
	if (IS_ERR(core_ck))
		goto err2;
	if (clk_set_parent(psSysSpecData->psSGX_FCK, core_ck) < 0) {
		clk_put(core_ck);
		goto err2;
	}
	clk_put(core_ck);

	RegisterConstraintNotifications(psSysSpecData);

	return PVRSRV_OK;

err2:
	clk_put(psSysSpecData->psSGX_ICK);
err1:
	clk_put(psSysSpecData->psSGX_FCK);
err0:
	PVR_DPF(PVR_DBG_ERROR,
		 "%s: couldn't init clocks fck %p ick %p core %p", __func__,
		 psSysSpecData->psSGX_FCK, psSysSpecData->psSGX_ICK, core_ck);
	psSysSpecData->psSGX_FCK = NULL;
	psSysSpecData->psSGX_ICK = NULL;

	return PVRSRV_ERROR_GENERIC;
}

static void CleanupSgxClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	UnRegisterConstraintNotifications(psSysSpecData);

	if (psSysSpecData->psSGX_ICK) {
		clk_put(psSysSpecData->psSGX_ICK);
		psSysSpecData->psSGX_ICK = NULL;
	}

	if (psSysSpecData->psSGX_FCK) {
		clk_put(psSysSpecData->psSGX_FCK);
		psSysSpecData->psSGX_FCK = NULL;
	}
}

#if defined(DEBUG) || defined(TIMING)
static inline u32 gpt_read_reg(struct SYS_DATA *psSysData, u32 reg)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	return __raw_readl(psSysSpecData->gpt_base + reg);
}

static inline void gpt_write_reg(struct SYS_DATA *psSysData, u32 reg, u32 val)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	__raw_writel(val, psSysSpecData->gpt_base + reg);
}

static enum PVRSRV_ERROR InitDebugClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;
	struct clk *psCLK;
	struct clk *sys_ck = NULL;
	u32 rate;

	psCLK = clk_get(NULL, "mpu_ck");
	if (IS_ERR(psCLK))
		goto err0;
	psSysSpecData->psMPU_CK = psCLK;

	psCLK = clk_get(NULL, "gpt11_fck");
	if (IS_ERR(psCLK))
		goto err1;
	psSysSpecData->psGPT11_FCK = psCLK;

	psCLK = clk_get(NULL, "gpt11_ick");
	if (IS_ERR(psCLK))
		goto err2;
	psSysSpecData->psGPT11_ICK = psCLK;

	sys_ck = clk_get(NULL, "sys_ck");
	if (IS_ERR(sys_ck))
		goto err3;
	if (clk_get_parent(psSysSpecData->psGPT11_FCK) != sys_ck)
		if (clk_set_parent(psSysSpecData->psGPT11_FCK, sys_ck) < 0) {
			clk_put(sys_ck);
			goto err3;
		}
	clk_put(sys_ck);

	PVR_TRACE("GPTIMER11 clock is %dMHz",
		   HZ_TO_MHZ(clk_get_rate(psSysSpecData->psGPT11_FCK)));

	psSysSpecData->gpt_base = ioremap(SYS_OMAP3430_GP11TIMER_PHYS_BASE,
					  SYS_OMAP3430_GPTIMER_SIZE);
	if (!psSysSpecData->gpt_base)
		goto err3;

	clk_enable(psSysSpecData->psGPT11_ICK);
	clk_enable(psSysSpecData->psGPT11_FCK);

	rate = gpt_read_reg(psSysData, SYS_OMAP3430_GPTIMER_TSICR);
	if (!(rate & 4)) {
		PVR_TRACE(
		 "Setting GPTIMER11 mode to posted (currently is non-posted)");
		gpt_write_reg(psSysData, SYS_OMAP3430_GPTIMER_TSICR, rate | 4);
	}

	clk_disable(psSysSpecData->psGPT11_FCK);
	clk_disable(psSysSpecData->psGPT11_ICK);

	return PVRSRV_OK;

err3:
	clk_put(psSysSpecData->psGPT11_ICK);
err2:
	clk_put(psSysSpecData->psGPT11_FCK);
err1:
	clk_put(psSysSpecData->psMPU_CK);
err0:
	PVR_DPF(PVR_DBG_ERROR,
		 "%s: couldn't init clocks: mpu %p sys %p fck %p ick %p",
		 __func__, psSysSpecData->psMPU_CK, sys_ck,
		 psSysSpecData->psGPT11_FCK, psSysSpecData->psGPT11_ICK);

	psSysSpecData->psMPU_CK = NULL;
	psSysSpecData->psGPT11_FCK = NULL;
	psSysSpecData->psGPT11_ICK = NULL;

	return PVRSRV_ERROR_GENERIC;
}

static void CleanupDebugClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	if (psSysSpecData->psMPU_CK) {
		clk_put(psSysSpecData->psMPU_CK);
		psSysSpecData->psMPU_CK = NULL;
	}
	if (psSysSpecData->psGPT11_FCK) {
		clk_put(psSysSpecData->psGPT11_FCK);
		psSysSpecData->psGPT11_FCK = NULL;
	}
	if (psSysSpecData->psGPT11_ICK) {
		clk_put(psSysSpecData->psGPT11_ICK);
		psSysSpecData->psGPT11_ICK = NULL;
	}
}

static enum PVRSRV_ERROR EnableDebugClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	if (clk_enable(psSysSpecData->psGPT11_FCK) < 0)
		goto err0;

	if (clk_enable(psSysSpecData->psGPT11_ICK) < 0)
		goto err1;

	gpt_write_reg(psSysData, SYS_OMAP3430_GPTIMER_ENABLE, 3);

	return PVRSRV_OK;

err1:
	clk_disable(psSysSpecData->psGPT11_FCK);
err0:
	PVR_DPF(PVR_DBG_ERROR, "%s: can't enable clocks", __func__);

	return PVRSRV_ERROR_GENERIC;
}

static inline void DisableDebugClocks(struct SYS_DATA *psSysData)
{
	struct SYS_SPECIFIC_DATA *psSysSpecData = psSysData->pvSysSpecificData;

	gpt_write_reg(psSysData, SYS_OMAP3430_GPTIMER_ENABLE, 0);

	clk_disable(psSysSpecData->psGPT11_ICK);
	clk_disable(psSysSpecData->psGPT11_FCK);
}

#else

inline enum PVRSRV_ERROR InitDebugClocks(struct SYS_DATA *psSysData)
{
	return PVRSRV_OK;
}

static inline void CleanupDebugClocks(struct SYS_DATA *psSysData)
{
}

static inline enum PVRSRV_ERROR EnableDebugClocks(struct SYS_DATA *psSysData)
{
	return PVRSRV_OK;
}

static inline void DisableDebugClocks(struct SYS_DATA *psSysData)
{
}
#endif

enum PVRSRV_ERROR InitSystemClocks(struct SYS_DATA *psSysData)
{
	if (InitSgxClocks(psSysData) != PVRSRV_OK)
		goto err0;

	if (InitDebugClocks(psSysData) != PVRSRV_OK)
		goto err1;

	return PVRSRV_OK;

err1:
	CleanupSgxClocks(psSysData);
err0:
	return PVRSRV_ERROR_GENERIC;
}

void CleanupSystemClocks(struct SYS_DATA *psSysData)
{
	CleanupDebugClocks(psSysData);
	CleanupSgxClocks(psSysData);
}

enum PVRSRV_ERROR EnableSystemClocks(struct SYS_DATA *psSysData)
{
	PVR_TRACE("EnableSystemClocks: Enabling System Clocks");


	if (EnableDebugClocks(psSysData) != PVRSRV_OK)
		goto err1;

	return PVRSRV_OK;

err1:
	return PVRSRV_ERROR_GENERIC;
}

void DisableSystemClocks(struct SYS_DATA *psSysData)
{
	PVR_TRACE("DisableSystemClocks: Disabling System Clocks");

	DisableSGXClocks(psSysData);
	DisableDebugClocks(psSysData);
}
