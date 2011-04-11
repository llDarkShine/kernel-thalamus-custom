/*
 *  drivers/cpufreq/cpufreq_hybrid.c
 *
 *  Copyright (C)  2011 Michal Potrzebicz <m.potrzebicz@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/init.h>

static int cpufreq_governor_hybrid(struct cpufreq_policy *policy, unsigned int event)
{
    switch (event) {
	case CPUFREQ_GOV_START:
	    break;
	case CPUFREQ_GOV_LIMITS:
	    break;
	default:
	    break;
    }
    return 0;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_HYBRID
static
#endif
struct cpufreq_governor cpufreq_gov_hybrid = {
    .name	= "hybrid",
    .governor	= cpufreq_governor_hybrid,
    .owner	= THIS_MODULE,
};


static int __init cpufreq_gov_hybrid_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_hybrid);
}


static void __exit cpufreq_gov_hybrid_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_hybrid);
}


MODULE_AUTHOR("Michal Potrzebicz <m.potrzebicz@gmail.com>");
MODULE_DESCRIPTION("CPUfreq policy governor 'hybrid'");
MODULE_LICENSE("GPL");

fs_initcall(cpufreq_gov_hybrid_init);
module_exit(cpufreq_gov_hybrid_exit);
