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
#include <asm/cputime.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/tick.h>

#define TRANSITION_LATENCY_LIMIT	(10 * 1000 * 1000)

static atomic_t active_count = ATOMIC_INIT(0);

struct cpufreq_hybrid_cpuinfo {
	struct cpufreq_policy *policy;
	struct timer_list timer;
	
	unsigned int target_freq;
	u64 prev_idle_time;
	u64 prev_wall_time;
	u64 last_freq_change;
};

static DEFINE_PER_CPU(struct cpufreq_hybrid_cpuinfo, cpuinfo);

#define DEFAULT_SAMPLE_RATE		(2)
#define DEFAULT_UP_THRESHOLD		(80)
#define DEFAULT_DOWN_THRESHOLD		(30)
#define DEFAULT_UP_STEP			(10)
#define DEFAULT_DOWN_DELAY		(50000)
#define DEFAULT_DOWN_DIFFERENTIAL	(10)

struct cpufreq_hybrid_tuners {
    unsigned int sample_rate;
    unsigned int up_threshold;
    unsigned int down_threshold;
    unsigned int up_step;
    unsigned int down_delay;
    unsigned int down_differential;
} tuners = {
    .sample_rate	= DEFAULT_SAMPLE_RATE,
    .up_threshold 	= DEFAULT_UP_THRESHOLD,
    .down_threshold	= DEFAULT_DOWN_THRESHOLD,
    .up_step		= DEFAULT_UP_STEP,
    .down_delay		= DEFAULT_DOWN_DELAY,
    .down_differential	= DEFAULT_DOWN_DIFFERENTIAL,
};

static void cpufreq_hybrid_timer(unsigned long data)
{
	u64 idle_time;
	u64 wall_time;
	unsigned int delta_idle_time;
	unsigned int delta_wall_time;
	unsigned int perc_load;
	struct cpufreq_hybrid_cpuinfo *this_cpuinfo = &per_cpu(cpuinfo, data);
	struct cpufreq_policy *policy = this_cpuinfo->policy;

	idle_time = get_cpu_idle_time_us(data, &wall_time);
	delta_idle_time = (unsigned int) cputime64_sub(idle_time, this_cpuinfo->prev_idle_time);
	delta_wall_time = (unsigned int) cputime64_sub(wall_time, this_cpuinfo->prev_wall_time);
	this_cpuinfo->prev_idle_time = idle_time;
	this_cpuinfo->prev_wall_time = wall_time;
	
	if (delta_idle_time > delta_wall_time)
		perc_load = 0;
	else
		perc_load = (100 * (delta_wall_time - delta_idle_time)) / delta_wall_time;
	
	if (perc_load > tuners.up_threshold) {
	
		this_cpuinfo->target_freq += (tuners.up_step * policy->max) / 100;
		if (this_cpuinfo->target_freq > policy->max)
			this_cpuinfo->target_freq = policy->max;
		this_cpuinfo->last_freq_change = wall_time;
		
	} else if ((perc_load < tuners.down_threshold) && 
		    (cputime64_sub(wall_time, this_cpuinfo->last_freq_change) > tuners.down_delay)) {
		    
		this_cpuinfo->target_freq = (perc_load * policy->cur) / (tuners.up_threshold - tuners.down_differential);
		if (this_cpuinfo->target_freq < policy->min)
			this_cpuinfo->target_freq = policy->min;
		this_cpuinfo->last_freq_change = wall_time;
		
	}
	
	// Schedule next sample
	if (!timer_pending(&this_cpuinfo->timer))
		mod_timer(&this_cpuinfo->timer, jiffies + tuners.sample_rate);
}

static int cpufreq_governor_hybrid(struct cpufreq_policy *policy, unsigned int event)
{
	struct cpufreq_hybrid_cpuinfo *this_cpuinfo = &per_cpu(cpuinfo, policy->cpu);
		
	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(policy->cpu)) || (!policy->cur))
			return -EINVAL;
			
		this_cpuinfo->policy = policy;
		this_cpuinfo->target_freq = policy->cur;
		
		// create sysfs entries when first governor is started
		if (atomic_inc_return(&active_count) == 1) {
			// create sysfs entries here
		}
		
		break;
		
	case CPUFREQ_GOV_STOP:
		// remove sysfs entries when last governor is stopped
		if (atomic_dec_and_test(&active_count)) {
			// remove sysfs entries here
		}
		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
		    __cpufreq_driver_target(policy, policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
		    __cpufreq_driver_target(policy, policy->min, CPUFREQ_RELATION_L);
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
	.name			= "hybrid",	
	.governor		= cpufreq_governor_hybrid,
	.max_transition_latency = TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_hybrid_init(void)
{
	unsigned int i;
	struct cpufreq_hybrid_cpuinfo *new_cpuinfo;
	
	// Initialize CPU timers
	for_each_possible_cpu(i) {
		new_cpuinfo = &per_cpu(cpuinfo, i);
		init_timer_deferrable(&new_cpuinfo->timer);
		new_cpuinfo->timer.function = cpufreq_hybrid_timer;
		new_cpuinfo->timer.data = i;
	}
	
	return cpufreq_register_governor(&cpufreq_gov_hybrid);
}

static void __exit cpufreq_gov_hybrid_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_hybrid);
}

MODULE_AUTHOR("Michal Potrzebicz <m.potrzebicz@gmail.com>");
MODULE_DESCRIPTION("CPUfreq policy governor 'hybrid'");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_HYBRID
fs_initcall(cpufreq_gov_hybrid_init);
#else
module_init(cpufreq_gov_hybrid_init);
#endif
module_exit(cpufreq_gov_hybrid_exit);
