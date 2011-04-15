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
#include <linux/workqueue.h>
#include <linux/slab.h>

#define TRANSITION_LATENCY_LIMIT	(10 * 1000 * 1000)

static atomic_t active_count = ATOMIC_INIT(0);

struct cpufreq_hybrid_cpuinfo {
	struct cpufreq_policy *policy;
	struct timer_list timer;
	u64 prev_idle_time;
	u64 prev_wall_time;
	unsigned long last_freq_change;
	unsigned int enabled;
};

static DEFINE_PER_CPU(struct cpufreq_hybrid_cpuinfo, cpuinfo);

typedef struct {
	struct work_struct work;

	struct cpufreq_policy *policy;
	unsigned int target_freq;
	unsigned int relation;
} cpufreq_work_struct;

static struct workqueue_struct *up_queue;
static struct workqueue_struct *down_queue;

#define DEFAULT_SAMPLE_RATE		(2)
#define DEFAULT_DOWN_DELAY		(4)
#define DEFAULT_UP_THRESHOLD		(80)
#define DEFAULT_DOWN_THRESHOLD		(40)

struct cpufreq_hybrid_tuners {
    unsigned int sample_rate;
    unsigned int down_delay;
    unsigned int up_threshold;
    unsigned int down_threshold;
    unsigned int optimal_load;
} tuners = {
    .sample_rate	= DEFAULT_SAMPLE_RATE,
    .down_delay		= DEFAULT_DOWN_DELAY,
    .up_threshold 	= DEFAULT_UP_THRESHOLD,
    .down_threshold	= DEFAULT_DOWN_THRESHOLD,
};

static void cpufreq_hybrid_scale_work( struct work_struct *work )
{
	cpufreq_work_struct *scale_work = (cpufreq_work_struct *) work;

//	printk( KERN_DEBUG "Executing CPUFreq scale work\n");
	__cpufreq_driver_target(scale_work->policy, scale_work->target_freq, scale_work->relation);

	kfree((void *)work);
}

static void cpufreq_hybrid_enqueue_scale_work( struct cpufreq_policy *policy, unsigned int target_freq, unsigned int relation )
{
	cpufreq_work_struct *work = (cpufreq_work_struct *)kmalloc(sizeof(cpufreq_work_struct), GFP_KERNEL);
	if (work) {
		INIT_WORK((struct work_struct *)work, cpufreq_hybrid_scale_work);
		work->policy = policy;
		work->target_freq = target_freq;
		work->relation = relation;
		if (relation == CPUFREQ_RELATION_H)
			queue_work(up_queue, (struct work_struct *)work);
		else
			queue_work(down_queue, (struct work_struct *)work);
	}
}

static void cpufreq_hybrid_timer( unsigned long data )
{
	u64 idle_time;
	u64 wall_time;
	unsigned int delta_idle_time;
	unsigned int delta_wall_time;
	unsigned int perc_load;
	unsigned int target_freq;
	struct cpufreq_hybrid_cpuinfo *this_cpuinfo = &per_cpu(cpuinfo, data);
	struct cpufreq_policy *policy = this_cpuinfo->policy;

	if (!this_cpuinfo->enabled)
	    return;

	idle_time = get_cpu_idle_time_us(data, &wall_time);
	delta_idle_time = (unsigned int) cputime64_sub(idle_time, this_cpuinfo->prev_idle_time);
	delta_wall_time = (unsigned int) cputime64_sub(wall_time, this_cpuinfo->prev_wall_time);
	this_cpuinfo->prev_idle_time = idle_time;
	this_cpuinfo->prev_wall_time = wall_time;

	// calculate load percentage
	if (delta_idle_time > delta_wall_time)
		perc_load = 0;
	else
		perc_load = (100 * (delta_wall_time - delta_idle_time)) / delta_wall_time;

	if ((perc_load > tuners.up_threshold) && (policy->cur != policy->max)) {

		// calculate optimal frequency
		target_freq = (perc_load * policy->cur) / tuners.optimal_load;
		if (target_freq > policy->max)
			target_freq = policy->max;
		this_cpuinfo->last_freq_change = jiffies;

//		printk(KERN_DEBUG "CPUFreq UP - perc_load: %u target_freq: %u\n", perc_load, target_freq);
		cpufreq_hybrid_enqueue_scale_work(policy, target_freq, CPUFREQ_RELATION_H);
		
	} else if ((perc_load < tuners.down_threshold) && (policy->cur != policy->min) &&
		((jiffies - this_cpuinfo->last_freq_change) > tuners.down_delay)) {

		// calculate optimal lower frequency
		target_freq = (perc_load * policy->cur) / tuners.optimal_load;
		if (target_freq < policy->min)
			target_freq = policy->min;
		this_cpuinfo->last_freq_change = jiffies;

//		printk(KERN_DEBUG "CPUFreq DOWN - perc_load: %u target_freq: %u\n", perc_load, target_freq);
		cpufreq_hybrid_enqueue_scale_work(policy, target_freq, CPUFREQ_RELATION_L);
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
		this_cpuinfo->prev_idle_time = get_cpu_idle_time_us(policy->cpu, &this_cpuinfo->prev_wall_time);
		this_cpuinfo->last_freq_change = 0;
		this_cpuinfo->enabled = 0;
		// sample timer initialization
		init_timer_deferrable(&this_cpuinfo->timer);
		this_cpuinfo->timer.function = cpufreq_hybrid_timer;
		this_cpuinfo->timer.data = policy->cpu;

		// create sysfs entries when first governor is started
		if (atomic_inc_return(&active_count) == 1) {
			// create sysfs entries here
		}
		this_cpuinfo->enabled = 1;
		mod_timer(&this_cpuinfo->timer, jiffies + tuners.sample_rate);
		break;

	case CPUFREQ_GOV_STOP:
		this_cpuinfo->enabled = 0;
		del_timer_sync(&this_cpuinfo->timer);
		// remove sysfs entries when last governor is stopped
		if (atomic_dec_and_test(&active_count)) {
			// remove sysfs entries here
		}
		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < this_cpuinfo->policy->max)
		    __cpufreq_driver_target(this_cpuinfo->policy, policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_cpuinfo->policy->cur)
		    __cpufreq_driver_target(this_cpuinfo->policy, policy->min, CPUFREQ_RELATION_L);
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
	tuners.optimal_load = tuners.up_threshold - 10;

	up_queue = create_rt_workqueue("khybrid_up");
	down_queue = create_workqueue("khybrid_down");

	return cpufreq_register_governor(&cpufreq_gov_hybrid);
}

static void __exit cpufreq_gov_hybrid_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_hybrid);

	destroy_workqueue(up_queue);
	destroy_workqueue(down_queue);
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
