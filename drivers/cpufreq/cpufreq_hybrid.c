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
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/init.h>
#include <linux/workqueue.h>

#define TRANSITION_LATENCY_LIMIT	(10 * 1000 * 1000)

static atomic_t active_count = ATOMIC_INIT(0);

struct cpufreq_hybrid_cpuinfo {
	struct cpufreq_policy *policy;
	struct delayed_work work;
	u64 prev_idle_time;
	u64 prev_wall_time;
	unsigned long last_freq_change;
	unsigned long full_load_samples;
	unsigned long optimal_load;
};

static DEFINE_PER_CPU(struct cpufreq_hybrid_cpuinfo, cpuinfo);

static struct workqueue_struct *work_queue;

#define DEFAULT_SAMPLE_RATE		(2) // jiffies
#define DEFAULT_DOWN_DELAY_SAMPLES	(0)
#define DEFAULT_UP_THRESHOLD		(90)
#define DEFAULT_DOWN_THRESHOLD		(30)
#define DEFAULT_MAX_FULL_LOAD_SAMPLES	(1)
#define DEFAULT_OPTIMAL_LOAD_CORRECTION	(5)

#define MIN_LATENCY_MULTIPLIER		(100)
#define LATENCY_MULTIPLIER		(1000)

struct cpufreq_hybrid_tuners {
    unsigned int sample_rate;
    unsigned int down_delay_samples;
    unsigned int up_threshold;
    unsigned int down_threshold;
    unsigned int max_full_load_samples;
    unsigned int optimal_load;
    unsigned int optimal_load_correction;
} tuners = {
    .sample_rate	= DEFAULT_SAMPLE_RATE,
    .down_delay_samples	= DEFAULT_DOWN_DELAY_SAMPLES,
    .up_threshold 	= DEFAULT_UP_THRESHOLD,
    .down_threshold	= DEFAULT_DOWN_THRESHOLD,
    .max_full_load_samples = DEFAULT_MAX_FULL_LOAD_SAMPLES,
    .optimal_load_correction = DEFAULT_OPTIMAL_LOAD_CORRECTION,
};

static void cpufreq_hybrid_work( struct work_struct *work )
{
	u64 idle_time;
	u64 wall_time;
	unsigned int delta_idle_time;
	unsigned int delta_wall_time;
	unsigned int perc_load;
	unsigned int target_freq;
	unsigned int cpu = smp_processor_id();
	struct cpufreq_hybrid_cpuinfo *this_cpuinfo = &per_cpu(cpuinfo, cpu);
	struct cpufreq_policy *policy = this_cpuinfo->policy;

	// sample data
	idle_time = get_cpu_idle_time_us(cpu, &wall_time);
	delta_idle_time = (unsigned int) cputime64_sub(idle_time, this_cpuinfo->prev_idle_time);
	delta_wall_time = (unsigned int) cputime64_sub(wall_time, this_cpuinfo->prev_wall_time);

	// calculate load percentage
	if (delta_idle_time > delta_wall_time)
		perc_load = 0;
	else
		perc_load = (100 * (delta_wall_time - delta_idle_time)) / delta_wall_time;

	if (((perc_load > tuners.up_threshold) && (policy->cur < policy->max)) ||
	    ((perc_load < tuners.down_threshold) && (policy->cur > policy->min) &&
		time_after(jiffies, this_cpuinfo->last_freq_change +
		    (tuners.down_delay_samples * tuners.sample_rate)))) {

		// calculate optimal frequency
		if (perc_load == 100)
			this_cpuinfo->full_load_samples++;
		else
			this_cpuinfo->full_load_samples = 0;

		if ((perc_load == 100) && (this_cpuinfo->full_load_samples >= tuners.max_full_load_samples)) {
			this_cpuinfo->optimal_load -= tuners.optimal_load_correction;
			if (this_cpuinfo->optimal_load < (tuners.down_threshold + 10))
				this_cpuinfo->optimal_load = tuners.down_threshold + 10;
		} else
			this_cpuinfo->optimal_load = tuners.optimal_load;

		target_freq = (perc_load * policy->cur) / this_cpuinfo->optimal_load;

		if (target_freq > policy->max)
			target_freq = policy->max;
		if (target_freq < policy->min)
			target_freq = policy->min;

		// we want to get at least the frequency we calculated
		// therefore CPUFREQ_RELATION_L is used in all cases
		// (see linux/cpufreq.h)
		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_L);
		this_cpuinfo->last_freq_change = jiffies;
	}

	// Schedule next sample
	this_cpuinfo->prev_idle_time = get_cpu_idle_time_us(cpu, &this_cpuinfo->prev_wall_time);
	queue_delayed_work_on(cpu, work_queue, &this_cpuinfo->work, tuners.sample_rate);
}

static int cpufreq_governor_hybrid(struct cpufreq_policy *policy, unsigned int event)
{
	struct cpufreq_hybrid_cpuinfo *this_cpuinfo = &per_cpu(cpuinfo, policy->cpu);
		
	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(policy->cpu)) || (!policy->cur))
			return -EINVAL;

		printk(KERN_DEBUG "Starting hybrid governor for cpu %u\n", policy->cpu);
		this_cpuinfo->policy = policy;
		this_cpuinfo->prev_idle_time = get_cpu_idle_time_us(policy->cpu, &this_cpuinfo->prev_wall_time);
		this_cpuinfo->last_freq_change = 0;
		this_cpuinfo->full_load_samples = 0;
		this_cpuinfo->optimal_load = tuners.optimal_load;

		// create sysfs entries when first governor is started
		if (atomic_inc_return(&active_count) == 1) {
			unsigned int latency;
			unsigned int min_sampling_rate;

			// policy latency is in ns. Convert it to us
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;
			printk(KERN_DEBUG "CPU latency = %u us\n", latency);
			min_sampling_rate = jiffies_to_usecs(tuners.sample_rate);
			printk(KERN_DEBUG "Default sampling rate = %u us (%u jiffies)\n", min_sampling_rate, tuners.sample_rate);

			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate, MIN_LATENCY_MULTIPLIER * latency);
			tuners.sample_rate = usecs_to_jiffies(
			    max(min_sampling_rate, latency * LATENCY_MULTIPLIER));
			printk(KERN_DEBUG "Setting sample rate to %u jiffies\n", tuners.sample_rate);
			// create sysfs entries here
		}

		INIT_DELAYED_WORK_DEFERRABLE(&this_cpuinfo->work, cpufreq_hybrid_work);
		queue_delayed_work_on(policy->cpu, work_queue, &this_cpuinfo->work, tuners.sample_rate);
		break;

	case CPUFREQ_GOV_STOP:
		printk(KERN_DEBUG "Stopping hybrid governor for cpu %u\n", policy->cpu);
		cancel_delayed_work_sync(&this_cpuinfo->work);

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

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_HYBRID
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

	work_queue = create_rt_workqueue("khybrid");
	return cpufreq_register_governor(&cpufreq_gov_hybrid);
}

static void __exit cpufreq_gov_hybrid_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_hybrid);
	destroy_workqueue(work_queue);
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
