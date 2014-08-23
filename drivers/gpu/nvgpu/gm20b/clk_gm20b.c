/*
 * GM20B Clocks
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/delay.h>	/* for mdelay */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/clk/tegra.h>

#include "gk20a/gk20a.h"
#include "hw_trim_gm20b.h"
#include "hw_timer_gm20b.h"
#include "hw_therm_gm20b.h"
#include "clk_gm20b.h"

#define gk20a_dbg_clk(fmt, arg...) \
	gk20a_dbg(gpu_dbg_clk, fmt, ##arg)

/* from vbios PLL info table */
static struct pll_parms gpc_pll_params = {
	128000,  2600000,	/* freq */
	1300000, 2600000,	/* vco */
	12000,   38400,		/* u */
	1, 255,			/* M */
	8, 255,			/* N */
	1, 31,			/* PL */
};

#ifdef CONFIG_DEBUG_FS
static int clk_gm20b_debugfs_init(struct gk20a *g);
#endif

/* 1:1 match between post divider settings and divisor value */
static inline u32 pl_to_div(u32 pl)
{
	return pl;
}

static inline u32 div_to_pl(u32 div)
{
	return div;
}

/* FIXME: remove after on-silicon testing */
#define PLDIV_GLITCHLESS 1

/* Calculate and update M/N/PL as well as pll->freq
    ref_clk_f = clk_in_f;
    u_f = ref_clk_f / M;
    vco_f = u_f * N = ref_clk_f * N / M;
    PLL output = gpc2clk = target clock frequency = vco_f / pl_to_pdiv(PL);
    gpcclk = gpc2clk / 2; */
static int clk_config_pll(struct clk_gk20a *clk, struct pll *pll,
	struct pll_parms *pll_params, u32 *target_freq, bool best_fit)
{
	u32 min_vco_f, max_vco_f;
	u32 best_M, best_N;
	u32 low_PL, high_PL, best_PL;
	u32 m, n, n2;
	u32 target_vco_f, vco_f;
	u32 ref_clk_f, target_clk_f, u_f;
	u32 delta, lwv, best_delta = ~0;
	u32 pl;

	BUG_ON(target_freq == NULL);

	gk20a_dbg_fn("request target freq %d MHz", *target_freq);

	ref_clk_f = pll->clk_in;
	target_clk_f = *target_freq;
	max_vco_f = pll_params->max_vco;
	min_vco_f = pll_params->min_vco;
	best_M = pll_params->max_M;
	best_N = pll_params->min_N;
	best_PL = pll_params->min_PL;

	target_vco_f = target_clk_f + target_clk_f / 50;
	if (max_vco_f < target_vco_f)
		max_vco_f = target_vco_f;

	/* Set PL search boundaries. */
	high_PL = div_to_pl((max_vco_f + target_vco_f - 1) / target_vco_f);
	high_PL = min(high_PL, pll_params->max_PL);
	high_PL = max(high_PL, pll_params->min_PL);

	low_PL = div_to_pl(min_vco_f / target_vco_f);
	low_PL = min(low_PL, pll_params->max_PL);
	low_PL = max(low_PL, pll_params->min_PL);

	gk20a_dbg_info("low_PL %d(div%d), high_PL %d(div%d)",
			low_PL, pl_to_div(low_PL), high_PL, pl_to_div(high_PL));

	for (pl = low_PL; pl <= high_PL; pl++) {
		target_vco_f = target_clk_f * pl_to_div(pl);

		for (m = pll_params->min_M; m <= pll_params->max_M; m++) {
			u_f = ref_clk_f / m;

			if (u_f < pll_params->min_u)
				break;
			if (u_f > pll_params->max_u)
				continue;

			n = (target_vco_f * m) / ref_clk_f;
			n2 = ((target_vco_f * m) + (ref_clk_f - 1)) / ref_clk_f;

			if (n > pll_params->max_N)
				break;

			for (; n <= n2; n++) {
				if (n < pll_params->min_N)
					continue;
				if (n > pll_params->max_N)
					break;

				vco_f = ref_clk_f * n / m;

				if (vco_f >= min_vco_f && vco_f <= max_vco_f) {
					lwv = (vco_f + (pl_to_div(pl) / 2))
						/ pl_to_div(pl);
					delta = abs(lwv - target_clk_f);

					if (delta < best_delta) {
						best_delta = delta;
						best_M = m;
						best_N = n;
						best_PL = pl;

						if (best_delta == 0 ||
						    /* 0.45% for non best fit */
						    (!best_fit && (vco_f / best_delta > 218))) {
							goto found_match;
						}

						gk20a_dbg_info("delta %d @ M %d, N %d, PL %d",
							delta, m, n, pl);
					}
				}
			}
		}
	}

found_match:
	BUG_ON(best_delta == ~0);

	if (best_fit && best_delta != 0)
		gk20a_dbg_clk("no best match for target @ %dMHz on gpc_pll",
			target_clk_f);

	pll->M = best_M;
	pll->N = best_N;
	pll->PL = best_PL;

	/* save current frequency */
	pll->freq = ref_clk_f * pll->N / (pll->M * pl_to_div(pll->PL));

	*target_freq = pll->freq;

	gk20a_dbg_clk("actual target freq %d MHz, M %d, N %d, PL %d(div%d)",
		*target_freq, pll->M, pll->N, pll->PL, pl_to_div(pll->PL));

	gk20a_dbg_fn("done");

	return 0;
}

static void clk_setup_slide(struct gk20a *g, u32 clk_u)
{
	u32 data, step_a, step_b;

	switch (clk_u) {
	case 12000:
	case 12800:
	case 13000:			/* only on FPGA */
		step_a = 0x2B;
		step_b = 0x0B;
		break;
	case 19200:
		step_a = 0x12;
		step_b = 0x08;
		break;
	case 38400:
		step_a = 0x04;
		step_b = 0x05;
		break;
	default:
		gk20a_err(dev_from_gk20a(g), "Unexpected reference rate %u kHz",
			  clk_u);
		BUG();
	}

	/* setup */
	data = gk20a_readl(g, trim_sys_gpcpll_cfg2_r());
	data = set_field(data, trim_sys_gpcpll_cfg2_pll_stepa_m(),
			trim_sys_gpcpll_cfg2_pll_stepa_f(step_a));
	gk20a_writel(g, trim_sys_gpcpll_cfg2_r(), data);
	data = gk20a_readl(g, trim_sys_gpcpll_cfg3_r());
	data = set_field(data, trim_sys_gpcpll_cfg3_pll_stepb_m(),
			trim_sys_gpcpll_cfg3_pll_stepb_f(step_b));
	gk20a_writel(g, trim_sys_gpcpll_cfg3_r(), data);
}

static int clk_slide_gpc_pll(struct gk20a *g, u32 n)
{
	u32 data, coeff;
	u32 nold, m;
	int ramp_timeout = 500;

	/* get old coefficients */
	coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	nold = trim_sys_gpcpll_coeff_ndiv_v(coeff);

	/* do nothing if NDIV is same */
	if (n == nold)
		return 0;

	/* dynamic ramp setup based on update rate */
	m = trim_sys_gpcpll_coeff_mdiv_v(coeff);
	clk_setup_slide(g, g->clk.gpc_pll.clk_in / m);

	/* pll slowdown mode */
	data = gk20a_readl(g, trim_sys_gpcpll_ndiv_slowdown_r());
	data = set_field(data,
			trim_sys_gpcpll_ndiv_slowdown_slowdown_using_pll_m(),
			trim_sys_gpcpll_ndiv_slowdown_slowdown_using_pll_yes_f());
	gk20a_writel(g, trim_sys_gpcpll_ndiv_slowdown_r(), data);

	/* new ndiv ready for ramp */
	coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	coeff = set_field(coeff, trim_sys_gpcpll_coeff_ndiv_m(),
			trim_sys_gpcpll_coeff_ndiv_f(n));
	udelay(1);
	gk20a_writel(g, trim_sys_gpcpll_coeff_r(), coeff);

	/* dynamic ramp to new ndiv */
	data = gk20a_readl(g, trim_sys_gpcpll_ndiv_slowdown_r());
	data = set_field(data,
			trim_sys_gpcpll_ndiv_slowdown_en_dynramp_m(),
			trim_sys_gpcpll_ndiv_slowdown_en_dynramp_yes_f());
	udelay(1);
	gk20a_writel(g, trim_sys_gpcpll_ndiv_slowdown_r(), data);

	do {
		udelay(1);
		ramp_timeout--;
		data = gk20a_readl(
			g, trim_gpc_bcast_gpcpll_ndiv_slowdown_debug_r());
		if (trim_gpc_bcast_gpcpll_ndiv_slowdown_debug_pll_dynramp_done_synced_v(data))
			break;
	} while (ramp_timeout > 0);

	/* exit slowdown mode */
	data = gk20a_readl(g, trim_sys_gpcpll_ndiv_slowdown_r());
	data = set_field(data,
			trim_sys_gpcpll_ndiv_slowdown_slowdown_using_pll_m(),
			trim_sys_gpcpll_ndiv_slowdown_slowdown_using_pll_no_f());
	data = set_field(data,
			trim_sys_gpcpll_ndiv_slowdown_en_dynramp_m(),
			trim_sys_gpcpll_ndiv_slowdown_en_dynramp_no_f());
	gk20a_writel(g, trim_sys_gpcpll_ndiv_slowdown_r(), data);
	gk20a_readl(g, trim_sys_gpcpll_ndiv_slowdown_r());

	if (ramp_timeout <= 0) {
		gk20a_err(dev_from_gk20a(g), "gpcpll dynamic ramp timeout");
		return -ETIMEDOUT;
	}
	return 0;
}

static int clk_lock_gpc_pll_under_bypass(struct gk20a *g, u32 m, u32 n, u32 pl)
{
	u32 data, cfg, coeff, timeout;

	/* put PLL in bypass before programming it */
	data = gk20a_readl(g, trim_sys_sel_vco_r());
	data = set_field(data, trim_sys_sel_vco_gpc2clk_out_m(),
		trim_sys_sel_vco_gpc2clk_out_bypass_f());
	gk20a_writel(g, trim_sys_sel_vco_r(), data);

	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	if (trim_sys_gpcpll_cfg_iddq_v(cfg)) {
		/* get out from IDDQ (1st power up) */
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_iddq_m(),
				trim_sys_gpcpll_cfg_iddq_power_on_v());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
		gk20a_readl(g, trim_sys_gpcpll_cfg_r());
		udelay(5);
	} else {
		/* clear SYNC_MODE before disabling PLL */
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_sync_mode_m(),
				trim_sys_gpcpll_cfg_sync_mode_disable_f());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
		gk20a_readl(g, trim_sys_gpcpll_cfg_r());

		/* disable running PLL before changing coefficients */
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
				trim_sys_gpcpll_cfg_enable_no_f());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
		gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	}

	/* change coefficients */
	coeff = trim_sys_gpcpll_coeff_mdiv_f(m) |
		trim_sys_gpcpll_coeff_ndiv_f(n) |
		trim_sys_gpcpll_coeff_pldiv_f(pl);
	gk20a_writel(g, trim_sys_gpcpll_coeff_r(), coeff);

	/* enable PLL after changing coefficients */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
			trim_sys_gpcpll_cfg_enable_yes_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);

	/* lock pll */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	if (cfg & trim_sys_gpcpll_cfg_enb_lckdet_power_off_f()){
		cfg = set_field(cfg, trim_sys_gpcpll_cfg_enb_lckdet_m(),
			trim_sys_gpcpll_cfg_enb_lckdet_power_on_f());
		gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
	}

	/* wait pll lock */
	timeout = g->clk.pll_delay / 2 + 1;
	do {
		cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
		if (cfg & trim_sys_gpcpll_cfg_pll_lock_true_f())
			goto pll_locked;
		udelay(2);
	} while (--timeout > 0);

	/* PLL is messed up. What can we do here? */
	BUG();
	return -EBUSY;

pll_locked:
	gk20a_dbg_clk("locked config_pll under bypass r=0x%x v=0x%x",
		trim_sys_gpcpll_cfg_r(), cfg);

	/* set SYNC_MODE for glitchless switch out of bypass */
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_sync_mode_m(),
			trim_sys_gpcpll_cfg_sync_mode_enable_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
	gk20a_readl(g, trim_sys_gpcpll_cfg_r());

	/* put PLL back on vco */
	data = gk20a_readl(g, trim_sys_sel_vco_r());
	data = set_field(data, trim_sys_sel_vco_gpc2clk_out_m(),
		trim_sys_sel_vco_gpc2clk_out_vco_f());
	gk20a_writel(g, trim_sys_sel_vco_r(), data);

	return 0;
}

static int clk_program_gpc_pll(struct gk20a *g, struct clk_gk20a *clk,
			int allow_slide)
{
#if PLDIV_GLITCHLESS
	bool skip_bypass;
#else
	u32 data;
#endif
	u32 cfg, coeff;
	u32 m, n, pl, nlo;
	bool can_slide;

	gk20a_dbg_fn("");

	if (!tegra_platform_is_silicon())
		return 0;

	/* get old coefficients */
	coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	m = trim_sys_gpcpll_coeff_mdiv_v(coeff);
	n = trim_sys_gpcpll_coeff_ndiv_v(coeff);
	pl = trim_sys_gpcpll_coeff_pldiv_v(coeff);

	/* do NDIV slide if there is no change in M and PL */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	can_slide = allow_slide && trim_sys_gpcpll_cfg_enable_v(cfg);

	if (can_slide && (clk->gpc_pll.M == m) && (clk->gpc_pll.PL == pl))
		return clk_slide_gpc_pll(g, clk->gpc_pll.N);

	/* slide down to NDIV_LO */
	nlo = DIV_ROUND_UP(m * gpc_pll_params.min_vco, clk->gpc_pll.clk_in);
	if (can_slide) {
		int ret = clk_slide_gpc_pll(g, nlo);
		if (ret)
			return ret;
	}

#if PLDIV_GLITCHLESS
	/*
	 * Limit either FO-to-FO (path A below) or FO-to-bypass (path B below)
	 * jump to min_vco/2 by setting post divider >= 1:2.
	 */
	skip_bypass = can_slide && (clk->gpc_pll.M == m);
	coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	if ((skip_bypass && (clk->gpc_pll.PL < 2)) || (pl < 2)) {
		if (pl != 2) {
			coeff = set_field(coeff,
				trim_sys_gpcpll_coeff_pldiv_m(),
				trim_sys_gpcpll_coeff_pldiv_f(2));
			gk20a_writel(g, trim_sys_gpcpll_coeff_r(), coeff);
			coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
			udelay(2);
		}
	}

	if (skip_bypass)
		goto set_pldiv;	/* path A: no need to bypass */

	/* path B: bypass if either M changes or PLL is disabled */
#else
	/* split FO-to-bypass jump in halfs by setting out divider 1:2 */
	data = gk20a_readl(g, trim_sys_gpc2clk_out_r());
	data = set_field(data, trim_sys_gpc2clk_out_vcodiv_m(),
		trim_sys_gpc2clk_out_vcodiv_f(2));
	gk20a_writel(g, trim_sys_gpc2clk_out_r(), data);
	gk20a_readl(g, trim_sys_gpc2clk_out_r());
	udelay(2);
#endif
	/*
	 * Program and lock pll under bypass. On exit PLL is out of bypass,
	 * enabled, and locked. VCO is at vco_min if sliding is allowed.
	 * Otherwise it is at VCO target (and therefore last slide call below
	 * is effectively NOP). PL is preserved (not set to target) of post
	 * divider is glitchless. Otherwise it is at PL target.
	 */
	m = clk->gpc_pll.M;
	nlo = DIV_ROUND_UP(m * gpc_pll_params.min_vco, clk->gpc_pll.clk_in);
	n = allow_slide ? nlo : clk->gpc_pll.N;
#if PLDIV_GLITCHLESS
	pl = (clk->gpc_pll.PL < 2) ? 2 : clk->gpc_pll.PL;
#else
	pl = clk->gpc_pll.PL;
#endif
	clk_lock_gpc_pll_under_bypass(g, m, n, pl);
	clk->gpc_pll.enabled = true;

#if PLDIV_GLITCHLESS
	coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	udelay(2);

set_pldiv:
	/* coeff must be current from either path A or B */
	if (trim_sys_gpcpll_coeff_pldiv_v(coeff) != clk->gpc_pll.PL) {
		coeff = set_field(coeff, trim_sys_gpcpll_coeff_pldiv_m(),
			trim_sys_gpcpll_coeff_pldiv_f(clk->gpc_pll.PL));
		gk20a_writel(g, trim_sys_gpcpll_coeff_r(), coeff);
	}
#else
	/* restore out divider 1:1 */
	data = gk20a_readl(g, trim_sys_gpc2clk_out_r());
	data = set_field(data, trim_sys_gpc2clk_out_vcodiv_m(),
		trim_sys_gpc2clk_out_vcodiv_by1_f());
	udelay(2);
	gk20a_writel(g, trim_sys_gpc2clk_out_r(), data);
#endif
	/* slide up to target NDIV */
	return clk_slide_gpc_pll(g, clk->gpc_pll.N);
}

static int clk_disable_gpcpll(struct gk20a *g, int allow_slide)
{
	u32 cfg, coeff, m, nlo;
	struct clk_gk20a *clk = &g->clk;

	/* slide to VCO min */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	if (allow_slide && trim_sys_gpcpll_cfg_enable_v(cfg)) {
		coeff = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
		m = trim_sys_gpcpll_coeff_mdiv_v(coeff);
		nlo = DIV_ROUND_UP(m * gpc_pll_params.min_vco,
				   clk->gpc_pll.clk_in);
		clk_slide_gpc_pll(g, nlo);
	}

	/* put PLL in bypass before disabling it */
	cfg = gk20a_readl(g, trim_sys_sel_vco_r());
	cfg = set_field(cfg, trim_sys_sel_vco_gpc2clk_out_m(),
			trim_sys_sel_vco_gpc2clk_out_bypass_f());
	gk20a_writel(g, trim_sys_sel_vco_r(), cfg);

	/* clear SYNC_MODE before disabling PLL */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_sync_mode_m(),
			trim_sys_gpcpll_cfg_sync_mode_disable_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);

	/* disable PLL */
	cfg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	cfg = set_field(cfg, trim_sys_gpcpll_cfg_enable_m(),
			trim_sys_gpcpll_cfg_enable_no_f());
	gk20a_writel(g, trim_sys_gpcpll_cfg_r(), cfg);
	gk20a_readl(g, trim_sys_gpcpll_cfg_r());

	clk->gpc_pll.enabled = false;
	return 0;
}

static int gm20b_init_clk_reset_enable_hw(struct gk20a *g)
{
	gk20a_dbg_fn("");
	return 0;
}

struct clk *gm20b_clk_get(struct gk20a *g)
{
	if (!g->clk.tegra_clk) {
		struct clk *clk;

		clk = clk_get_sys("tegra_gk20a", "gpu");
		if (IS_ERR(clk)) {
			gk20a_err(dev_from_gk20a(g),
				"fail to get tegra gpu clk tegra_gk20a/gpu");
			return NULL;
		}
		g->clk.tegra_clk = clk;
	}

	return g->clk.tegra_clk;
}

static int gm20b_init_clk_setup_sw(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	static int initialized;
	struct clk *ref;
	unsigned long ref_rate;

	gk20a_dbg_fn("");

	if (clk->sw_ready) {
		gk20a_dbg_fn("skip init");
		return 0;
	}

	if (!gk20a_clk_get(g))
		return -EINVAL;

	ref = clk_get_parent(clk_get_parent(clk->tegra_clk));
	if (IS_ERR(ref)) {
		gk20a_err(dev_from_gk20a(g),
			"failed to get GPCPLL reference clock");
		return -EINVAL;
	}
	ref_rate = clk_get_rate(ref);

	clk->pll_delay = 300; /* usec */

	clk->gpc_pll.id = GK20A_GPC_PLL;
	clk->gpc_pll.clk_in = ref_rate / KHZ;

	/* Initial frequency: 1/3 VCO min (low enough to be safe at Vmin) */
	if (!initialized) {
		initialized = 1;
		clk->gpc_pll.M = 1;
		clk->gpc_pll.N = DIV_ROUND_UP(gpc_pll_params.min_vco,
					clk->gpc_pll.clk_in);
		clk->gpc_pll.PL = 3;
		clk->gpc_pll.freq = clk->gpc_pll.clk_in * clk->gpc_pll.N;
		clk->gpc_pll.freq /= pl_to_div(clk->gpc_pll.PL);
	}

	mutex_init(&clk->clk_mutex);

	clk->sw_ready = true;

	gk20a_dbg_fn("done");
	return 0;
}

static int gm20b_init_clk_setup_hw(struct gk20a *g)
{
	u32 data;

	gk20a_dbg_fn("");

	/* LDIV: Div4 mode (required); both  bypass and vco ratios 1:1 */
	data = gk20a_readl(g, trim_sys_gpc2clk_out_r());
	data = set_field(data,
			trim_sys_gpc2clk_out_sdiv14_m() |
			trim_sys_gpc2clk_out_vcodiv_m() |
			trim_sys_gpc2clk_out_bypdiv_m(),
			trim_sys_gpc2clk_out_sdiv14_indiv4_mode_f() |
			trim_sys_gpc2clk_out_vcodiv_by1_f() |
			trim_sys_gpc2clk_out_bypdiv_f(0));
	gk20a_writel(g, trim_sys_gpc2clk_out_r(), data);

	/*
	 * Clear global bypass control; PLL is still under bypass, since SEL_VCO
	 * is cleared by default.
	 */
	data = gk20a_readl(g, trim_sys_bypassctrl_r());
	data = set_field(data, trim_sys_bypassctrl_gpcpll_m(),
			 trim_sys_bypassctrl_gpcpll_vco_f());
	gk20a_writel(g, trim_sys_bypassctrl_r(), data);

	/* Disable idle slow down */
	data = gk20a_readl(g, therm_clk_slowdown_r(0));
	data = set_field(data, therm_clk_slowdown_idle_factor_m(),
			 therm_clk_slowdown_idle_factor_disabled_f());
	gk20a_writel(g, therm_clk_slowdown_r(0), data);
	gk20a_readl(g, therm_clk_slowdown_r(0));

	return 0;
}

static int set_pll_target(struct gk20a *g, u32 freq, u32 old_freq)
{
	struct clk_gk20a *clk = &g->clk;

	if (freq > gpc_pll_params.max_freq)
		freq = gpc_pll_params.max_freq;
	else if (freq < gpc_pll_params.min_freq)
		freq = gpc_pll_params.min_freq;

	if (freq != old_freq) {
		/* gpc_pll.freq is changed to new value here */
		if (clk_config_pll(clk, &clk->gpc_pll, &gpc_pll_params,
				   &freq, true)) {
			gk20a_err(dev_from_gk20a(g),
				   "failed to set pll target for %d", freq);
			return -EINVAL;
		}
	}
	return 0;
}

static int set_pll_freq(struct gk20a *g, u32 freq, u32 old_freq)
{
	struct clk_gk20a *clk = &g->clk;
	int err = 0;

	gk20a_dbg_fn("curr freq: %dMHz, target freq %dMHz", old_freq, freq);

	if ((freq == old_freq) && clk->gpc_pll.enabled)
		return 0;

	/* change frequency only if power is on */
	if (g->clk.clk_hw_on) {
		err = clk_program_gpc_pll(g, clk, 1);
		if (err)
			err = clk_program_gpc_pll(g, clk, 0);
	}

	/* Just report error but not restore PLL since dvfs could already change
	    voltage even when it returns error. */
	if (err)
		gk20a_err(dev_from_gk20a(g),
			"failed to set pll to %d", freq);
	return err;
}

static int gm20b_clk_export_set_rate(void *data, unsigned long *rate)
{
	u32 old_freq;
	int ret = -ENODATA;
	struct gk20a *g = data;
	struct clk_gk20a *clk = &g->clk;

	if (rate) {
		mutex_lock(&clk->clk_mutex);
		old_freq = clk->gpc_pll.freq;
		ret = set_pll_target(g, rate_gpu_to_gpc2clk(*rate), old_freq);
		if (!ret && clk->gpc_pll.enabled)
			ret = set_pll_freq(g, clk->gpc_pll.freq, old_freq);
		if (!ret)
			*rate = rate_gpc2clk_to_gpu(clk->gpc_pll.freq);
		mutex_unlock(&clk->clk_mutex);
	}
	return ret;
}

static int gm20b_clk_export_enable(void *data)
{
	int ret;
	struct gk20a *g = data;
	struct clk_gk20a *clk = &g->clk;

	mutex_lock(&clk->clk_mutex);
	ret = set_pll_freq(g, clk->gpc_pll.freq, clk->gpc_pll.freq);
	mutex_unlock(&clk->clk_mutex);
	return ret;
}

static void gm20b_clk_export_disable(void *data)
{
	struct gk20a *g = data;
	struct clk_gk20a *clk = &g->clk;

	mutex_lock(&clk->clk_mutex);
	if (g->clk.clk_hw_on)
		clk_disable_gpcpll(g, 1);
	mutex_unlock(&clk->clk_mutex);
}

static void gm20b_clk_export_init(void *data, unsigned long *rate, bool *state)
{
	struct gk20a *g = data;
	struct clk_gk20a *clk = &g->clk;

	mutex_lock(&clk->clk_mutex);
	if (state)
		*state = clk->gpc_pll.enabled;
	if (rate)
		*rate = rate_gpc2clk_to_gpu(clk->gpc_pll.freq);
	mutex_unlock(&clk->clk_mutex);
}

static struct tegra_clk_export_ops gm20b_clk_export_ops = {
	.init = gm20b_clk_export_init,
	.enable = gm20b_clk_export_enable,
	.disable = gm20b_clk_export_disable,
	.set_rate = gm20b_clk_export_set_rate,
};

static int gm20b_clk_register_export_ops(struct gk20a *g)
{
	int ret;
	struct clk *c;

	if (gm20b_clk_export_ops.data)
		return 0;

	gm20b_clk_export_ops.data = (void *)g;
	c = g->clk.tegra_clk;
	if (!c || !clk_get_parent(c))
		return -ENOSYS;

	ret = tegra_clk_register_export_ops(clk_get_parent(c),
					    &gm20b_clk_export_ops);

	return ret;
}

static int gm20b_init_clk_support(struct gk20a *g)
{
	struct clk_gk20a *clk = &g->clk;
	u32 err;

	gk20a_dbg_fn("");

	clk->g = g;

	err = gm20b_init_clk_reset_enable_hw(g);
	if (err)
		return err;

	err = gm20b_init_clk_setup_sw(g);
	if (err)
		return err;

	mutex_lock(&clk->clk_mutex);
	clk->clk_hw_on = true;

	err = gm20b_init_clk_setup_hw(g);
	mutex_unlock(&clk->clk_mutex);
	if (err)
		return err;

	err = gm20b_clk_register_export_ops(g);
	if (err)
		return err;

	/* FIXME: this effectively prevents host level clock gating */
	err = clk_enable(g->clk.tegra_clk);
	if (err)
		return err;

	/* The prev call may not enable PLL if gbus is unbalanced - force it */
	mutex_lock(&clk->clk_mutex);
	err = set_pll_freq(g, clk->gpc_pll.freq, clk->gpc_pll.freq);
	mutex_unlock(&clk->clk_mutex);
	if (err)
		return err;

#ifdef CONFIG_DEBUG_FS
	if (!clk->debugfs_set) {
		if (!clk_gm20b_debugfs_init(g))
			clk->debugfs_set = true;
	}
#endif
	return err;
}

static int gm20b_suspend_clk_support(struct gk20a *g)
{
	int ret;

	clk_disable(g->clk.tegra_clk);

	/* The prev call may not disable PLL if gbus is unbalanced - force it */
	mutex_lock(&g->clk.clk_mutex);
	ret = clk_disable_gpcpll(g, 1);
	g->clk.clk_hw_on = false;
	mutex_unlock(&g->clk.clk_mutex);
	return ret;
}

void gm20b_init_clk_ops(struct gpu_ops *gops)
{
	gops->clk.init_clk_support = gm20b_init_clk_support;
	gops->clk.suspend_clk_support = gm20b_suspend_clk_support;
}

#ifdef CONFIG_DEBUG_FS

static int rate_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	*val = (u64)gk20a_clk_get_rate(g);
	return 0;
}
static int rate_set(void *data, u64 val)
{
	struct gk20a *g = (struct gk20a *)data;
	return gk20a_clk_set_rate(g, (u32)val);
}
DEFINE_SIMPLE_ATTRIBUTE(rate_fops, rate_get, rate_set, "%llu\n");

static int pll_reg_show(struct seq_file *s, void *data)
{
	struct gk20a *g = s->private;
	u32 reg, m, n, pl, f;

	mutex_lock(&g->clk.clk_mutex);
	if (!g->clk.clk_hw_on) {
		seq_printf(s, "gk20a powered down - no access to registers\n");
		mutex_unlock(&g->clk.clk_mutex);
		return 0;
	}

	reg = gk20a_readl(g, trim_sys_bypassctrl_r());
	seq_printf(s, "bypassctrl = %s, ", reg ? "bypass" : "vco");
	reg = gk20a_readl(g, trim_sys_sel_vco_r());
	seq_printf(s, "sel_vco = %s, ", reg ? "vco" : "bypass");

	reg = gk20a_readl(g, trim_sys_gpcpll_cfg_r());
	seq_printf(s, "cfg  = 0x%x : %s : %s : %s\n", reg,
		trim_sys_gpcpll_cfg_enable_v(reg) ? "enabled" : "disabled",
		trim_sys_gpcpll_cfg_pll_lock_v(reg) ? "locked" : "unlocked",
		trim_sys_gpcpll_cfg_sync_mode_v(reg) ? "sync_on" : "sync_off");

	reg = gk20a_readl(g, trim_sys_gpcpll_coeff_r());
	m = trim_sys_gpcpll_coeff_mdiv_v(reg);
	n = trim_sys_gpcpll_coeff_ndiv_v(reg);
	pl = trim_sys_gpcpll_coeff_pldiv_v(reg);
	f = g->clk.gpc_pll.clk_in * n / (m * pl_to_div(pl));
	seq_printf(s, "coef = 0x%x : m = %u : n = %u : pl = %u", reg, m, n, pl);
	seq_printf(s, " : pll_f(gpu_f) = %u(%u) kHz\n", f, f/2);
	mutex_unlock(&g->clk.clk_mutex);
	return 0;
}

static int pll_reg_open(struct inode *inode, struct file *file)
{
	return single_open(file, pll_reg_show, inode->i_private);
}

static const struct file_operations pll_reg_fops = {
	.open		= pll_reg_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int monitor_get(void *data, u64 *val)
{
	struct gk20a *g = (struct gk20a *)data;
	struct clk_gk20a *clk = &g->clk;
	u32 clk_slowdown, clk_slowdown_save;
	int err;

	u32 ncycle = 100; /* count GPCCLK for ncycle of clkin */
	u64 freq = clk->gpc_pll.clk_in;
	u32 count1, count2;

	err = gk20a_busy(g->dev);
	if (err)
		return err;

	mutex_lock(&g->clk.clk_mutex);

	/* Disable clock slowdown during measurements */
	clk_slowdown_save = gk20a_readl(g, therm_clk_slowdown_r(0));
	clk_slowdown = set_field(clk_slowdown_save,
				 therm_clk_slowdown_idle_factor_m(),
				 therm_clk_slowdown_idle_factor_disabled_f());
	gk20a_writel(g, therm_clk_slowdown_r(0), clk_slowdown);
	gk20a_readl(g, therm_clk_slowdown_r(0));

	gk20a_writel(g, trim_gpc_clk_cntr_ncgpcclk_cfg_r(0),
		     trim_gpc_clk_cntr_ncgpcclk_cfg_reset_asserted_f());
	gk20a_writel(g, trim_gpc_clk_cntr_ncgpcclk_cfg_r(0),
		     trim_gpc_clk_cntr_ncgpcclk_cfg_enable_asserted_f() |
		     trim_gpc_clk_cntr_ncgpcclk_cfg_write_en_asserted_f() |
		     trim_gpc_clk_cntr_ncgpcclk_cfg_noofipclks_f(ncycle));
	/* start */

	/* It should take less than 5us to finish 100 cycle of 38.4MHz.
	   But longer than 100us delay is required here. */
	gk20a_readl(g, trim_gpc_clk_cntr_ncgpcclk_cfg_r(0));
	udelay(200);

	count1 = gk20a_readl(g, trim_gpc_clk_cntr_ncgpcclk_cnt_r(0));
	udelay(100);
	count2 = gk20a_readl(g, trim_gpc_clk_cntr_ncgpcclk_cnt_r(0));
	freq *= trim_gpc_clk_cntr_ncgpcclk_cnt_value_v(count2);
	do_div(freq, ncycle);
	*val = freq;

	/* Restore clock slowdown */
	gk20a_writel(g, therm_clk_slowdown_r(0), clk_slowdown_save);
	mutex_unlock(&g->clk.clk_mutex);

	gk20a_idle(g->dev);

	if (count1 != count2)
		return -EBUSY;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(monitor_fops, monitor_get, NULL, "%llu\n");

static int clk_gm20b_debugfs_init(struct gk20a *g)
{
	struct dentry *d;
	struct gk20a_platform *platform = platform_get_drvdata(g->dev);

	d = debugfs_create_file(
		"rate", S_IRUGO|S_IWUSR, platform->debugfs, g, &rate_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"pll_reg", S_IRUGO, platform->debugfs, g, &pll_reg_fops);
	if (!d)
		goto err_out;

	d = debugfs_create_file(
		"monitor", S_IRUGO, platform->debugfs, g, &monitor_fops);
	if (!d)
		goto err_out;

	return 0;

err_out:
	pr_err("%s: Failed to make debugfs node\n", __func__);
	debugfs_remove_recursive(platform->debugfs);
	return -ENOMEM;
}

#endif /* CONFIG_DEBUG_FS */
