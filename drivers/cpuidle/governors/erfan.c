/*
 * erfan.c - Erfan Idle governor
 *
 *  Created on: May 10, 2018
 *      Author: Erfan Sharafzadeh
 */

#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/pm_qos.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/random.h>

#define US_TO_NS(x)	(x << 10)
#define INTERVALS 8
#define INTERVAL_SHIFT 3

struct erfan_device {
	int		last_state_idx;
	unsigned long	index;
	unsigned long timer_wake;
	unsigned long event_wake;
	unsigned long was_on_high_cstate;
	struct hrtimer hr_timer;
	int timer_active;
	unsigned int	intervals[INTERVALS];
	int		interval_ptr;
};

int counter = 0;


static DEFINE_PER_CPU(struct erfan_device, erfan_devices);

enum hrtimer_restart my_hrtimer_callback( struct hrtimer *timer )
{
	struct erfan_device *data = this_cpu_ptr(&erfan_devices);
	data->timer_active = 0;
	//printk_ratelimited("timer expired: before: %lu  after: %lu (%lu)\n", data->before_jiffies, data->after_jiffies, data->index);
	return HRTIMER_NORESTART;
}

void interval_business(struct erfan_device *data, unsigned int measured_us, int cpu){
	int i, divisor;
	unsigned int max, thresh;
	uint64_t avg, stddev;
	thresh = UINT_MAX; /* Discard outliers above this value */
	/* update the repeating-pattern data */
	data->intervals[data->interval_ptr++] = measured_us;
	if (data->interval_ptr >= INTERVALS)
		data->interval_ptr = 0;

	max = 0;
	avg = 0;
	divisor = 0;
	for (i = 0; i < INTERVALS; i++) {
		unsigned int value = data->intervals[i];
		if (value <= thresh) {
			avg += value;
			divisor++;
			if (value > max)
				max = value;
		}
	}
	if (divisor == INTERVALS)
		avg >>= INTERVAL_SHIFT;
	else
		do_div(avg, divisor);

	/* Then try to determine standard deviation */
	stddev = 0;
	for (i = 0; i < INTERVALS; i++) {
		unsigned int value = data->intervals[i];
		if (value <= thresh) {
			int64_t diff = value - avg;
			stddev += diff * diff;
		}
	}
	if (divisor == INTERVALS)
		stddev >>= INTERVAL_SHIFT;
	else
		do_div(stddev, divisor);
	printk_ratelimited("last residency= %d, average= %d  stddev= %d : cpu %d\n", measured_us, avg, stddev, cpu);
}



/**
 * erfan_select - selects the next idle state to enter
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
static int erfan_select(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct erfan_device *data = this_cpu_ptr(&erfan_devices);
	int throughput_req = pm_qos_request(PM_QOS_NETWORK_THROUGHPUT);
	int i, j, next_request;
	ktime_t ktime;
	unsigned long delay_in_us;
	unsigned int measured_us;
	measured_us = cpuidle_get_last_residency(dev);
	interval_business(data, measured_us, dev->cpu);
//	get_random_bytes(&j, sizeof(j));
//	lessthan100 = j % drv->state_count;
//	lessthan100++;
//	printk_ratelimited("qos = %d\n", throughput_req);
//
//	for (i = CPUIDLE_DRIVER_STATE_START; i < drv->state_count; i++) {
//			struct cpuidle_state *s = &drv->states[i];
//			struct cpuidle_state_usage *su = &dev->states_usage[i];
//
//			if (s->disabled || su->disable)
//				continue;
//			if (i == lessthan100){
//				data->last_state_idx = i;
//				break;
//			}
//
//			data->last_state_idx = i;
//	}
	data->index++;
	if(data->timer_active)
	{
		hrtimer_cancel(&data->hr_timer);
		data->timer_active = 0;
		data->event_wake++;
	}
	if(!throughput_req)
	{
		// TODO: Decide here
		data->last_state_idx = 4;
		goto out;
	}
	if(data->last_state_idx > 2)
	{
		data->last_state_idx = 0;
		data->was_on_high_cstate++;
		goto out;
	}
	next_request = div_u64(1000000, throughput_req);
	if(next_request > 200)
	{
		data->last_state_idx = 4;
		delay_in_us = next_request - 150;
	}
	else if(next_request > 100)
	{
		data->last_state_idx = 3;
		delay_in_us = next_request - 40;
	}
	else if(next_request > 40)
	{
		data->last_state_idx = 2;
		delay_in_us = next_request - 10;
	}
	else
	{
		data->last_state_idx = 0;
		goto out;
	}
	ktime = ktime_set( 0, US_TO_NS(delay_in_us));
	//printk_ratelimited( "Starting timer to fire in %ldus cstate: %d (%lu)\n", delay_in_us, data->last_state_idx, data->index);

	hrtimer_start( &data->hr_timer, ktime, HRTIMER_MODE_REL );
	data->timer_active = 1;
out:
	if(!(data->index % 5000))
		printk("cpu %d  index %lu  unmature wakeups: %lu, was on high cstate: %lu", dev->cpu, data->index, data->event_wake, data->was_on_high_cstate);
	return data->last_state_idx;
}

/**
 * erfan_reflect - records that data structures need update
 * @dev: the CPU
 * @index: the index of actual entered state
 *
 * NOTE: it's important to be fast here because this operation will add to
 *       the overall exit latency.
 */
static void erfan_reflect(struct cpuidle_device *dev, int index)
{
	struct erfan_device *data = this_cpu_ptr(&erfan_devices);

	data->last_state_idx = index;
}

/**
 * erfan_update - attempts to guess what happened after entry
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
//static void erfan_update(struct cpuidle_driver *drv, struct cpuidle_device *dev)
//{
//
//}

/**
 * erfan_enable_device - scans a CPU's states and does setup
 * @drv: cpuidle driver
 * @dev: the CPU
 */
static int erfan_enable_device(struct cpuidle_driver *drv,
				struct cpuidle_device *dev)
{
	struct erfan_device *data = &per_cpu(erfan_devices, dev->cpu);
	int i;

	memset(data, 0, sizeof(struct erfan_device));
	hrtimer_init( &data->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	data->hr_timer.function = &my_hrtimer_callback;
	return 0;
}

static struct cpuidle_governor erfan_governor = {
	.name =		"erfan",
	.rating =	30,
	.enable =	erfan_enable_device,
	.select =	erfan_select,
	.reflect =	erfan_reflect,
	.owner =	THIS_MODULE,
};

/**
 * init_erfan - initializes the governor
 */
static int __init init_erfan(void)
{
	return cpuidle_register_governor(&erfan_governor);
}

postcore_initcall(init_erfan);
