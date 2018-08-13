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
#include <linux/list_sort.h>
#include <net/sock.h>
#include "exp.h"
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>


#define EXPERT_NAME_LEN 15
#define ACTIVE_EXPERTS 2
#define INITIAL_WEIGHT 1000
#define US_TO_NS(x)	(x << 10)
#define INTERVALS 8
#define INTERVAL_SHIFT 3
#define EXPONENTIAL_FACTOR 18
#define EXPONENTIAL_FLOOR 20
#define BUCKETS 12
#define RESOLUTION 1024
#define DECAY 8
#define MAX_INTERESTING 50000

#define bool int
#define false 0
#define true 1

// ######################## Start of Data definitions ##############################################

static struct kobject *yawn_kobject;

struct yawn_device {
	// Yawn Global Data
	int		last_state_idx;
	unsigned int	next_timer_us;
	unsigned int	predicted_us;
	unsigned int	measured_us;
	unsigned int	pending;
	unsigned int attendees;
	struct hrtimer hr_timer;
	bool timer_active;
	bool woke_by_timer;
	bool needs_update;
	unsigned long inmature;
	unsigned long total;
	int expert_id_counter;
	unsigned int weights[ACTIVE_EXPERTS];
	int predictions[ACTIVE_EXPERTS];
	int former_predictions[ACTIVE_EXPERTS];
	bool will_wake_with_timer;
	bool strict_latency;
	bool network_activity;
	int idle_counter;
	int busy_counter;
	int deep_threshold;
	int shallow_threshold;
	// Residency Expert Data
	unsigned int residency_moving_average;
	// Network Expert Data
	struct timeval before;
	unsigned long last_ttwu_counter;
	unsigned int ttwu_rate;
	unsigned int last_cntxswch_counter;
	unsigned int cntxswch_rate;
	unsigned long epoll_events;
	unsigned long event_rate;
	unsigned long interarrival;

	// Timer Expert Data
	unsigned int	bucket;
	unsigned int	correction_factor[BUCKETS];

};

struct expert {
	 int id;
	 char name[EXPERT_NAME_LEN];
	 void (*init) (struct yawn_device *data, struct cpuidle_device *dev);
	 int (*select) (struct yawn_device *data, struct cpuidle_device *dev);
	 void (*reflect) (struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us);
	 struct list_head expert_list;
};

struct list_head expert_list;

static DEFINE_PER_CPU(struct yawn_device, yawn_devices);

// ######################## End of Data definitions ################################################

// ######################## Start of Yawn utility function definitions #############################

static void yawn_update(struct cpuidle_driver*, struct cpuidle_device*, struct yawn_device*);

enum hrtimer_restart my_hrtimer_callback( struct hrtimer *timer )
{
	struct yawn_device *data = this_cpu_ptr(&yawn_devices);
	data->timer_active = 0;
	if(!data->needs_update)
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
	unsigned int exit_latency = 0;
	unsigned int index = 0, sum = 0, i;
	int yawn_timer_interval;
	struct yawn_device *data = this_cpu_ptr(&yawn_devices);
	struct list_head *position = NULL ;
	struct expert  *expertptr  = NULL ;
	int state_count = drv->state_count;

	// reflect the last residency into experts and yawn
	if (data->needs_update) {
		yawn_update(drv, dev, data);
		data->needs_update = false;
	}
	data->network_activity = false;
	data->strict_latency = false;
	data->woke_by_timer = false;
	data->will_wake_with_timer = false;

	sched_reset_tasks_woke();
	data->total++;
	data->next_timer_us = ktime_to_us(tick_nohz_get_sleep_length());
	data->attendees = 0;
//	 query the experts for their delay prediction
	list_for_each ( position , &expert_list )
	{
		 expertptr = list_entry(position, struct expert, expert_list);
		 data->predictions[expertptr->id] = expertptr->select(data, dev);
		 if(data->predictions[expertptr->id] != -1)
		 {
			 //printk_ratelimited("select! expert %d is %d!\n", expertptr->id, data->predictions[expertptr->id]);
			 data->attendees++;
			 sum += data->weights[expertptr->id] * data->predictions[expertptr->id];
			 index+= data->weights[expertptr->id];
		 }
	}
	if(index == 0)
	{
		printk("BUG!!!!!!!!!!!!!!!!!!!!!\n");
		return 1;
	}
	data->predicted_us = sum / index;
	data->last_state_idx = CPUIDLE_DRIVER_STATE_START - 1;

//	printk_ratelimited("select! weights %d and %d! : predicted = %d\n", data->weights[0], data->weights[1], data->predicted_us);

	/*
	 * We want to default to C1 (hlt), not to busy polling
	 * unless the timer is happening really really soon.
	 */

	if (data->next_timer_us > 5 &&
		!drv->states[CPUIDLE_DRIVER_STATE_START].disabled &&
		dev->states_usage[CPUIDLE_DRIVER_STATE_START].disable == 0)
		data->last_state_idx = CPUIDLE_DRIVER_STATE_START;

	if(data->predicted_us > data->next_timer_us)
	{
		data->predicted_us = data->next_timer_us;
		data->will_wake_with_timer = true;
	}

	/*
	 * Find the idle state with the lowest power while satisfying
	 * our constraints.
	 */
	if(data->strict_latency)
		state_count--;

	for (i = CPUIDLE_DRIVER_STATE_START; i < state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];
		if (s->disabled || su->disable)
			continue;
		if (s->target_residency > data->predicted_us)
			continue;

		data->last_state_idx = i;
		exit_latency = s->exit_latency;
	}

	if(data->network_activity && !data->will_wake_with_timer)
	{
		yawn_timer_interval = data->predicted_us - exit_latency;
		if(yawn_timer_interval > 5)
		{
			ktime = ktime_set( 0, US_TO_NS(yawn_timer_interval));
			hrtimer_start( &data->hr_timer, ktime, HRTIMER_MODE_REL );
			data->timer_active = true;
		}
	}
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
	if(data->timer_active)
	{
		hrtimer_cancel(&data->hr_timer);
		data->timer_active = false;
		data->inmature++;
	}
	data->needs_update = true;
}

/**
 * yawn_update - attempts to guess what happened after entry
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 */
static void yawn_update(struct cpuidle_driver *drv, struct cpuidle_device *dev, struct yawn_device *data)
{
	unsigned int measured_us = cpuidle_get_last_residency(dev);
	unsigned int loss;
	struct list_head *position = NULL, *position_1 = NULL ;
	struct expert  *expertptr  = NULL ;
	unsigned int floor = 1, i;
	int last_idx = data->last_state_idx;
	struct cpuidle_state *target = &drv->states[last_idx];
//	printk_ratelimited("yawn_updt cpu %d state %d, residency= %u, exit = %u, timer = %u\n",dev->cpu, last_idx, measured_us, target->exit_latency, data->next_timer_us);
	if (measured_us > target->exit_latency)
		measured_us -= target->exit_latency;
	else // we don't want any inaccuracies, so just ignore it
	{
		data->pending = false;
		return;
	}
	if (measured_us > data->next_timer_us)
		measured_us = data->next_timer_us;

	if(data->woke_by_timer && !sched_get_tasks_woke())
	{
		data->pending += measured_us;
		return;
	}
	measured_us += data->pending;
	data->measured_us = measured_us;
	data->pending = false;
//printk_ratelimited("cpu(%u) maex w=%u, p=%d, netex w=%u, p=%d, sys_pred = %u, state=%d, sleep=%u next_timer=%u, total = %lu, inmature = %lu\n",
//		dev->cpu, data->weights[0], data->predictions[0],data->weights[1], data->predictions[1],
//		data->predicted_us,last_idx, data->measured_us, data->next_timer_us, data->total, data->inmature);

	if(data->attendees > 1)
	{
		floor--;
		list_for_each ( position_1 , &expert_list )
		{
			expertptr = list_entry(position_1, struct expert, expert_list);
			loss = abs(data->former_predictions[expertptr->id] - data->measured_us);
			if(loss > 999)
				loss = 999;
			floor += data->weights[expertptr->id] * EXP[loss];
		}
		floor /= 1000;
	}
	// Updating the weights of the experts and calling their reflection methods
	list_for_each ( position , &expert_list )
	{
		expertptr = list_entry(position, struct expert, expert_list);
		expertptr->reflect(data, dev, measured_us);
		if(data->attendees > 1 && data->predictions[expertptr->id] != -1)
		{
			//printk_ratelimited("update: floor = %u,  attendees = %u prediction %d = %d   measured = %u\n", floor, data->attendees, expertptr->id, data->predictions[expertptr->id], data->measured_us);
			loss = abs(data->predictions[expertptr->id] - data->measured_us);
			if(loss > 999)
				loss = 999;
			data->weights[expertptr->id] *= EXP[loss];
			if(floor == 0)
			{
				//printk("ERROR!!!!!!!!!!!!!!!!!!!!!\n");
				return;
			}
			data->weights[expertptr->id] /= floor;
			if(data->weights[expertptr->id] < 5)
			{
				data->weights[expertptr->id] = 5;
			}
		}
	}
	for(i = 0 ;i < ACTIVE_EXPERTS; i++)
		data->former_predictions[i] = data->predictions[i];

}

static void register_expert(struct expert *e, struct yawn_device *data)
{
	list_add(&(e->expert_list), &expert_list);
	e->id = data->expert_id_counter++;
	data->weights[e->id] = INITIAL_WEIGHT;
}

static void yawn_reset_weights(struct yawn_device *data)
{
	int i;
	for(i=0; i < ACTIVE_EXPERTS; i++)
	{
		data->weights[i] = INITIAL_WEIGHT;
	}
}

// ######################## End of of Yawn utility function definitions ###########################

// ######################## Start of Experts definition ###########################################

// ## Expert1: Residency Expert ------------------------


void residency_expert_init(struct yawn_device *data, struct cpuidle_device *dev)
{

}

int residency_expert_select(struct yawn_device *data, struct cpuidle_device *dev)
{
	return data->residency_moving_average;
}

void residency_expert_reflect(struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us)
{
	unsigned int ema = data->residency_moving_average;
	ema = (EXPONENTIAL_FACTOR * ema) + (EXPONENTIAL_FLOOR - EXPONENTIAL_FACTOR) * data->measured_us;
	ema /= EXPONENTIAL_FLOOR;
	data->residency_moving_average = ema;
}

struct expert residency_expert = {
		.name = "residency",
		.init = residency_expert_init,
		.reflect = residency_expert_reflect,
		.select = residency_expert_select,
		.expert_list = LIST_HEAD_INIT(residency_expert.expert_list)
};

// ## Expert2: Netwrok Rate Expert ------------------------


void network_expert_init(struct yawn_device *data, struct cpuidle_device *dev)
{
	do_gettimeofday(&data->before);
	data->last_ttwu_counter = sched_get_nr_ttwu(dev->cpu);
}

int network_expert_select(struct yawn_device *data, struct cpuidle_device *dev)
{
	unsigned long ttwups, period, difference, epoll_events, epl_diff, rate_sum;
	struct timeval after;
	unsigned int max, thresh;
	//int value = pm_qos_request(PM_QOS_NETWORK_THROUGHPUT);
	do_gettimeofday(&after);
	period = after.tv_sec * 1000000 + after.tv_usec;
	period -= 1000000 * data->before.tv_sec + data->before.tv_usec;

	if(period >= 500000)
	{
		// 1
		ttwups = sched_get_nr_ttwu(dev->cpu);
		difference = ttwups - data->last_ttwu_counter;
		data->ttwu_rate = difference*2;
//		printk_ratelimited("rate: next req=%u cpu(%u) period = %ld, ttwus now= %lu, before = %lu, difference = %lu\n", data->ttwu_rate, dev->cpu, period, ttwups, data->last_ttwu_counter, difference);
		data->last_ttwu_counter = ttwups;
		data->before = after;

		// 2
		max = sched_get_net_reqs();
		thresh = max - data->last_cntxswch_counter;
		data->cntxswch_rate = thresh*2;
		data->last_cntxswch_counter = max;

		// 3
		epoll_events = sched_get_epoll_events();
		epl_diff = epoll_events - data->epoll_events;
		data->event_rate = epl_diff*2;
		data->epoll_events = epoll_events;
//		printk_ratelimited("net expert: core(%u) epoll=%lu  sched=%u ttwu=%u\n", dev->cpu, data->event_rate, data->cntxswch_rate, data->ttwu_rate);

		rate_sum = data->event_rate + data->event_rate + data->cntxswch_rate;
		//	rate_sum *=3;
		if(rate_sum)
			data->interarrival = div_u64(1000000, rate_sum);
		if(dev->cpu != 0 && (!data->interarrival || data->interarrival > data->deep_threshold)){
			sched_change_rq_status(dev->cpu, 0);
		}
		else if(dev->cpu < (num_online_cpus()-1) && data->interarrival < data->shallow_threshold)
		{
			sched_change_rq_status(dev->cpu+1, 1);
		}
	}
//	printk_ratelimited("rate_sum = %lu   event = %lu   ttwu = %u \n", rate_sum, data->event_rate, data->ttwu_rate);

	if(data->interarrival && data->interarrival < data->deep_threshold){
		if(data->interarrival > 400)
			data->strict_latency = true;

		data->network_activity = true;
		return data->interarrival;
	}
	yawn_reset_weights(data);
	return -1;
}

void network_expert_reflect(struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us)
{

}

struct expert network_expert = {
		.name = "network",
		.init = network_expert_init,
		.reflect = network_expert_reflect,
		.select = network_expert_select,
		.expert_list = LIST_HEAD_INIT(network_expert.expert_list)
};


// ## Expert3: Timer Expert ------------------------

static inline int which_bucket(unsigned int duration, unsigned long nr_iowaiters)
{
	int bucket = 0;

	/*
	 * We keep two groups of stats; one with no
	 * IO pending, one without.
	 * This allows us to calculate
	 * E(duration)|iowait
	 */
	if (nr_iowaiters)
		bucket = BUCKETS/2;

	if (duration < 10)
		return bucket;
	if (duration < 100)
		return bucket + 1;
	if (duration < 1000)
		return bucket + 2;
	if (duration < 10000)
		return bucket + 3;
	if (duration < 100000)
		return bucket + 4;
	return bucket + 5;
}

void timer_expert_init(struct yawn_device *data, struct cpuidle_device *dev)
{

}

int timer_expert_select(struct yawn_device *data, struct cpuidle_device *dev)
{
	unsigned long nr_iowaiters, cpu_load, expert_prediction;
	get_iowait_load(&nr_iowaiters, &cpu_load);
	data->bucket = which_bucket(data->next_timer_us, nr_iowaiters);
	expert_prediction = DIV_ROUND_CLOSEST_ULL((uint64_t)data->next_timer_us *
						 data->correction_factor[data->bucket],
						 RESOLUTION * DECAY);
	return expert_prediction;
}

void timer_expert_reflect(struct yawn_device *data, struct cpuidle_device *dev, unsigned int measured_us)
{
	unsigned int new_factor;

	new_factor = data->correction_factor[data->bucket];
	new_factor -= new_factor / DECAY;

	if (data->next_timer_us > 0 && measured_us < MAX_INTERESTING)
		new_factor += RESOLUTION * measured_us / data->next_timer_us;
	else
		new_factor += RESOLUTION;
	if (DECAY == 1 && unlikely(new_factor == 0))
		new_factor = 1;

	data->correction_factor[data->bucket] = new_factor;

}

struct expert timer_expert = {
		.name = "timer",
		.init = timer_expert_init,
		.reflect = timer_expert_reflect,
		.select = timer_expert_select,
		.expert_list = LIST_HEAD_INIT(timer_expert.expert_list)
};

// ######################## End of Experts definition ###########################################
// ######################## Start of Sysfs definition ###########################################


static ssize_t yawn_show_deep_thresh(struct kobject *kobj, struct kobj_attribute *attr,
                      char *buf)
{
	struct yawn_device *data = &per_cpu(yawn_devices, 0);
	return sprintf(buf, "%d\n", data->deep_threshold);
}

static ssize_t yawn_store_deep_thresh(struct kobject *kobj, struct kobj_attribute *attr,
                      char *buf, size_t count)
{
	int i, val;
	sscanf(buf, "%du", &val);
	for(i=0; i < num_online_cpus();i++)
	{
		struct yawn_device *data = &per_cpu(yawn_devices, i);
		data->deep_threshold = val;
	}
	printk("Setting Deep state threashold to %d\n", val);
	return (ssize_t)count;
}

static ssize_t yawn_show_shallow_thresh(struct kobject *kobj, struct kobj_attribute *attr,
                      char *buf)
{
	struct yawn_device *data = &per_cpu(yawn_devices, this_cpu());
	return sprintf(buf, "%d\n", data->shallow_threshold);
}

static ssize_t yawn_store_shallow_thresh(struct kobject *kobj, struct kobj_attribute *attr,
                      char *buf, size_t count)
{
	int i, val;
	sscanf(buf, "%du", &val);
	for(i=0; i < num_online_cpus();i++)
	{
		struct yawn_device *data = &per_cpu(yawn_devices, i);
		data->shallow_threshold = val;
	}
	printk("Setting Shalow state threashold to %d\n", val);
	return (ssize_t)count;
}


static struct kobj_attribute yawn_attribute1 =__ATTR(deep_threashold, 0660, yawn_show_deep_thresh,
		yawn_store_deep_thresh);

static struct kobj_attribute yawn_attribute2 =__ATTR(shallow_threashold, 0660, yawn_show_shallow_thresh,
		yawn_store_shallow_thresh);

// ######################## End of Sysfs definition ###########################################
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
	INIT_LIST_HEAD(&expert_list);
	register_expert(&residency_expert, data);
	register_expert(&network_expert, data);
	//register_expert(&timer_expert, data);
	hrtimer_init( &data->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
	data->hr_timer.function = &my_hrtimer_callback;
	data->deep_threshold = 10000;
	data->shallow_threshold = 50;
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
	int error;
	yawn_kobject = kobject_create_and_add("yawn", kernel_kobj);
	error = sysfs_create_file(yawn_kobject, &yawn_attribute1.attr);
	if (error) {
		printk("failed to create the file in /sys/kernel/yawn/deep \n");
	}
	error = sysfs_create_file(yawn_kobject, &yawn_attribute2.attr);
	if (error) {
		printk("failed to create the file in /sys/kernel/yawn/shallow \n");
	}
	return cpuidle_register_governor(&yawn_governor);
}

postcore_initcall(init_yawn);

// ######################## End of Yawn initialization ###########################################
