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
};



//static inline int get_loadavg(unsigned long load)
//{
//	return LOAD_INT(load) * 10 + LOAD_FRAC(load) / 10;
//}

static DEFINE_PER_CPU(struct erfan_device, erfan_devices);



/**
 * erfan_select - selects the next idle state to enter
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
static int erfan_select(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct erfan_device *data = this_cpu_ptr(&erfan_devices);
	int latency_req = pm_qos_request(PM_QOS_CPU_DMA_LATENCY);
	int i, j, lessthan100;
	get_random_bytes(&j, sizeof(j));
	lessthan100 = j % drv->state_count;
	lessthan100++;
	for (i = CPUIDLE_DRIVER_STATE_START; i < drv->state_count; i++) {
			struct cpuidle_state *s = &drv->states[i];
			struct cpuidle_state_usage *su = &dev->states_usage[i];

			if (s->disabled || su->disable)
				continue;
			if (i == lessthan100){
				data->last_state_idx = i;
				break;
			}

			data->last_state_idx = i;
	}

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
