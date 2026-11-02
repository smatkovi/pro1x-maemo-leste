/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux's clock consumer API for msm.ko: the clocks of the GPU's device
 * tree node ("clocks", "clock-names"), driven natively by msm_freebsd_bsd.c
 * (the SM6115's GPU clock controller and GCC's GPU branches) as Linux's
 * gpucc-sm6115, clk-alpha-pll, clk-rcg2 and clk-branch do it. A clock the
 * node does not name is absent (-ENOENT), as in Linux.
 * (freebsd/gpu/msm-port.md)
 */

#ifndef	_MSM_LKPI_LINUX_CLK_H
#define	_MSM_LKPI_LINUX_CLK_H

#include <linux/device.h>
#include <linux/err.h>

struct clk;

struct clk_bulk_data {
	const char	*id;
	struct clk	*clk;
};

struct clk	*lkpi_msm_clk_get(struct device *, const char *, bool optional);
int	lkpi_msm_clk_bulk_get_all(struct device *, struct clk_bulk_data **);
int	clk_prepare_enable(struct clk *);
void	clk_disable_unprepare(struct clk *);
int	clk_set_rate(struct clk *, unsigned long);
unsigned long clk_get_rate(struct clk *);
long	clk_round_rate(struct clk *, unsigned long);
const char *__clk_get_name(const struct clk *);

#define	devm_clk_get(dev, id)		lkpi_msm_clk_get((dev), (id), false)
#define	devm_clk_get_optional(dev, id)	lkpi_msm_clk_get((dev), (id), true)
#define	clk_get(dev, id)		lkpi_msm_clk_get((dev), (id), false)
#define	devm_clk_bulk_get_all(dev, c)	lkpi_msm_clk_bulk_get_all((dev), (c))

static inline void
clk_put(struct clk *clk)
{
}

static inline int
clk_prepare(struct clk *clk)
{
	return (0);
}

static inline void
clk_unprepare(struct clk *clk)
{
}

static inline int
clk_enable(struct clk *clk)
{
	return (clk_prepare_enable(clk));
}

static inline void
clk_disable(struct clk *clk)
{
	clk_disable_unprepare(clk);
}

/* Linux's bulk enable: all or none, disabled in reverse */
static inline int
clk_bulk_prepare_enable(int n, const struct clk_bulk_data *c)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		if ((ret = clk_prepare_enable(c[i].clk)) != 0) {
			while (i-- > 0)
				clk_disable_unprepare(c[i].clk);
			return (ret);
		}
	}
	return (0);
}

static inline void
clk_bulk_disable_unprepare(int n, const struct clk_bulk_data *c)
{
	while (n-- > 0)
		clk_disable_unprepare(c[n].clk);
}

/* the DSP glue's (lkpi_platform.c's drivers): none optional is absent */
static inline int
devm_clk_bulk_get_optional(struct device *dev, int n, struct clk_bulk_data *c)
{
	int i;

	for (i = 0; i < n; i++) {
		c[i].clk = lkpi_msm_clk_get(dev, c[i].id, true);
		if (IS_ERR(c[i].clk))
			return (PTR_ERR(c[i].clk));
	}
	return (0);
}

#endif
