/*
 * yawn.c - Yawn Idle-state governor
 *
 *  Created on: June 27, 2018
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

#define EXPERT_NAME_LEN 15
#define US_TO_NS(x)	(x << 10)
#define INTERVALS 8
#define INTERVAL_SHIFT 3

// ######################## Start of Data definitions ##############################################

struct yawn_device {
	// Yawn Global Data
	int		last_state_idx;
	unsigned long	index;
	unsigned int	next_timer_us;
	unsigned int	predicted_us;
	struct hrtimer hr_timer;
	int timer_active;
	int woke_by_timer;
	int needs_update;
	int inmature;
	// Residency Expert Data
	unsigned int	intervals[INTERVALS];
	int		interval_ptr;
	int		moving_average;

};

struct expert {
	 char name[EXPERT_NAME_LEN];
	 unsigned int weight;
	 void (*init) (struct yawn_device *data, struct cpuidle_device *dev);
	 int (*select) (struct yawn_device *data, struct cpuidle_driver *drv, struct cpuidle_device *dev);
	 void (*reflect) (struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us);
	 void (*data);
	 struct list_head expert_list;
};

struct list_head expert_list;
//struct expert expert_list[2];

static DEFINE_PER_CPU(struct yawn_device, yawn_devices);

// ######################## End of Data definitions ################################################

// ######################## Start of Yawn utility function definitions #############################

static void yawn_update(struct cpuidle_driver*, struct cpuidle_device*, struct yawn_device*);

enum hrtimer_restart my_hrtimer_callback( struct hrtimer *timer )
{
	struct yawn_device *data = this_cpu_ptr(&yawn_devices);
	data->timer_active = 0;
	data->woke_by_timer = 1;
	return HRTIMER_NORESTART;
}

/**
 * yawn_select - selects the next idle state to enter
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
static int yawn_select(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	ktime_t ktime;
	unsigned int exit_latency;
	unsigned int index = 0, sum = 0, i, yawn_timer_interval;
	struct yawn_device *data = this_cpu_ptr(&yawn_devices);
	// reflect the last residency into experts and yawn
	if (data->needs_update) {
		yawn_update(drv, dev, data);
		data->needs_update = 0;
	}
	// did an inmature wake up happen? turn off the timer
	if(data->timer_active)
	{
		hrtimer_cancel(&data->hr_timer);
		data->timer_active = 0;
		data->inmature++;
	}
	// did we wake by yawn timer? then a request might nearly arrive. Go to polling and wait.
	if(data->woke_by_timer)
	{
		data->woke_by_timer = 0;
		data->last_state_idx = 0;
		goto out;
	}
//	 query the experts for their delay prediction
	struct list_head *position = NULL ;
	struct expert  *expertptr  = NULL ;
	list_for_each ( position , &expert_list )
	{
		 expertptr = list_entry(position, struct expert, expert_list);
		 sum += expertptr->select(data, drv, dev);
		 index++;
	}
//	sum = expert_list[0].select(data, drv, dev);
//	index++;
	data->predicted_us = sum / index;

	/*
	 * We want to default to C1 (hlt), not to busy polling
	 * unless the timer is happening really really soon.
	 */
	data->next_timer_us = ktime_to_us(tick_nohz_get_sleep_length());

	if (data->next_timer_us > 5 &&
		!drv->states[CPUIDLE_DRIVER_STATE_START].disabled &&
		dev->states_usage[CPUIDLE_DRIVER_STATE_START].disable == 0)
		data->last_state_idx = CPUIDLE_DRIVER_STATE_START;

	/*
	 * Find the idle state with the lowest power while satisfying
	 * our constraints.
	 */
	for (i = CPUIDLE_DRIVER_STATE_START; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];
		if (s->disabled || su->disable)
			continue;
		if (s->target_residency > data->predicted_us)
			continue;
//		if (s->exit_latency > latency_req)
//			continue;

		data->last_state_idx = i;
		exit_latency = s->exit_latency;
	}


	yawn_timer_interval = data->predicted_us - exit_latency;
	//printk_ratelimited("predicted = %u, yawn timer = %u\n", data->predicted_us, yawn_timer_interval);

	ktime = ktime_set( 0, US_TO_NS(yawn_timer_interval));

	hrtimer_start( &data->hr_timer, ktime, HRTIMER_MODE_REL );
	data->timer_active = 1;
out:
	return data->last_state_idx;
}

/**
 * yawn_reflect - records that data structures need update
 * @dev: the CPU
 * @index: the index of actual entered state
 *
 * NOTE: it's important to be fast here because this operation will add to
 *       the overall exit latency.
 */
static void yawn_reflect(struct cpuidle_device *dev, int index)
{
	struct yawn_device *data = this_cpu_ptr(&yawn_devices);

	data->last_state_idx = index;
	data->needs_update = 1;
}

/**
 * yawn_update - attempts to guess what happened after entry
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
static void yawn_update(struct cpuidle_driver *drv, struct cpuidle_device *dev, struct yawn_device *data)
{
	unsigned int measured_us = cpuidle_get_last_residency(dev);
	struct list_head *position = NULL ;
	struct expert  *expertptr  = NULL ;
	list_for_each ( position , &expert_list )
	{
		 expertptr = list_entry(position, struct expert, expert_list);
		 expertptr->reflect(data, dev, measured_us);
	}
//	expert_list[0].reflect(data, dev, measured_us);

}

static void register_expert(struct expert *e)
{
	list_add(&(e->expert_list), &expert_list);
}

// ######################## End of of Yawn utility function definitions ###########################

// ######################## Start of Experts definition ###########################################

// ## Expert1: Residency Expert ------------------------


void residency_expert_init(struct yawn_device *data, struct cpuidle_device *dev)
{

}

int residency_expert_select(struct yawn_device *data, struct cpuidle_device *dev)
{
	int i, divisor;
	unsigned int max, thresh;
	uint64_t avg, stddev;
	thresh = UINT_MAX; /* Discard outliers above this value */

	max = 0;
	avg = 0;
	divisor = 0;
	for (i = 0; i < INTERVALS; i++) {
		unsigned int value = data->intervals[i];
		//printk_ratelimited("intervals %d = % u \n", i, value);
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
	return avg;
}

void residency_expert_reflect(struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us)
{
	/* update the repeating-pattern data */
	data->intervals[data->interval_ptr++] = measured_us;
	if (data->interval_ptr >= INTERVALS)
		data->interval_ptr = 0;
}

struct expert residency_expert = {
		.name = "residency",
		.weight = 1,
		.init = residency_expert_init,
		.reflect = residency_expert_reflect,
		.select = residency_expert_select,
		.expert_list = LIST_HEAD_INIT(residency_expert.expert_list)
};

// ## Expert2: Netwrok Rate Expert ------------------------


void network_expert_init(struct yawn_device *data, struct cpuidle_device *dev)
{

}

int network_expert_select(struct yawn_device *data, struct cpuidle_device *dev)
{
	int throughput_req = pm_qos_request(PM_QOS_NETWORK_THROUGHPUT);
	unsigned int next_request = div_u64(1000000, throughput_req);

	return next_request;
}

void network_expert_reflect(struct yawn_device *data, struct cpuidle_device *dev)
{

}

struct expert network_expert = {
		.name = "network",
		.weight = 1,
		.init = network_expert_init,
		.reflect = network_expert_reflect,
		.select = network_expert_select,
		.expert_list = LIST_HEAD_INIT(network_expert.expert_list)
};

// ######################## End of Experts definition ###########################################

// ######################## Start of Yawn initialization ###########################################

/**
 * yawn_enable_device - scans a CPU's states and does setup
 * @drv: cpuidle driver
 * @dev: the CPU
 */
static int yawn_enable_device(struct cpuidle_driver *drv,
				struct cpuidle_device *dev)
{
	struct yawn_device *data = &per_cpu(yawn_devices, dev->cpu);

	memset(data, 0, sizeof(struct yawn_device));

	INIT_LIST_HEAD(expert_list);
	register_expert(&residency_expert);
//	expert_list[0] = residency_expert;
//	register_expert(&network_expert);

	hrtimer_init( &data->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	data->hr_timer.function = &my_hrtimer_callback;
	return 0;
}

static struct cpuidle_governor yawn_governor = {
	.name =		"yawn",
	.rating =	40,
	.enable =	yawn_enable_device,
	.select =	yawn_select,
	.reflect =	yawn_reflect,
	.owner =	THIS_MODULE,
};

/**
 * init_yawn - initializes the governor
 */
static int __init init_yawn(void)
{
	return cpuidle_register_governor(&yawn_governor);
}

postcore_initcall(init_yawn);

// ######################## End of Yawn initialization ###########################################
