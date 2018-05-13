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

struct erfan_device {
	int		last_state_idx;
	int             needs_update;
	unsigned int	next_timer_us;
	unsigned int	predicted_us;
	unsigned int	bucket;
	int		interval_ptr;
	// =e
	struct hrtimer hr_timer;
};

#define US_TO_NS(x)	(x * 1E3L)


//static inline int get_loadavg(unsigned long load)
//{
//	return LOAD_INT(load) * 10 + LOAD_FRAC(load) / 10;
//}

static DEFINE_PER_CPU(struct erfan_device, erfan_devices);

enum hrtimer_restart my_hrtimer_callback( struct hrtimer *timer )
{
  return HRTIMER_NORESTART;
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
	if(!throughput_req)
	{
		data->last_state_idx = 4;
		goto out;
	}
	next_request = 1e6l / throughput_req;
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
		data->last_state_idx = 3;
		delay_in_us = next_request - 10;
	}
	else
	{
		data->last_state_idx = 0;
		goto out;
	}
	ktime = ktime_set( 0, US_TO_NS(delay_in_us));
	printk_ratelimited( "Starting timer to fire in %ldus cstate: %d\n", delay_in_us, data->last_state_idx );

	hrtimer_start( &data->hr_timer, ktime, HRTIMER_MODE_REL );
out:
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
	data->needs_update = 1;
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
