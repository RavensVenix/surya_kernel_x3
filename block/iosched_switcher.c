/*
 * Copyright (C) 2017, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "iosched-swch: " fmt

#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/elevator.h>
#include <linux/fb.h>

#define ANXIETY_IOSCHED "anxiety"
#define RESTORE_DELAY_MS (5000)

struct req_queue_data {
	struct list_head list;
	struct request_queue *queue;
	char prev_e[ELV_NAME_MAX];
	bool using_anxiety;
};

static struct delayed_work restore_prev;
static struct delayed_work sleep_sched;
static DEFINE_SPINLOCK(init_lock);
static struct req_queue_data req_queues = {
	.list = LIST_HEAD_INIT(req_queues.list),
};

static void change_elevator(struct req_queue_data *r, bool use_anxiety)
{
	struct request_queue *q = r->queue;

	if (r-> using_anxiety == use_anxiety)
		return;

	r->using_anxiety = use_anxiety;

	if (use_anxiety) {
		strcpy(r->prev_e, q->elevator->type->elevator_name);
		elevator_init(q, ANXIETY_IOSCHED);
	} else {
		elevator_init(q, r->prev_e);
	}
}

static void change_all_elevators(struct list_head *head, bool use_anxiety)
{
	struct req_queue_data *r;

	list_for_each_entry(r, head, list)
		change_elevator(r, use_anxiety);
}

static int fb_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	switch (*blank) {
	case FB_BLANK_UNBLANK:
		/*
		 * Switch back from anxiety to the original iosched after a delay
		 * when the screen is turned on.
		 */
		if (delayed_work_pending(&sleep_sched))
			cancel_delayed_work_sync(&sleep_sched);
		schedule_delayed_work(&restore_prev,
				msecs_to_jiffies(RESTORE_DELAY_MS));
		break;
	default:
		/*
		 * Switch to anxiety when the screen turns off. Purposely block
		 * the fb notifier chain call in case weird things can happen
		 * when switching elevators while the screen is off.
		 */
		if (delayed_work_pending(&restore_prev))
			cancel_delayed_work_sync(&restore_prev);
		schedule_delayed_work(&sleep_sched,
				msecs_to_jiffies(RESTORE_DELAY_MS));
	}

	return NOTIFY_OK;
}

static struct notifier_block fb_notifier_callback_nb = {
	.notifier_call = fb_notifier_callback,
};

static void restore_prev_fn(struct work_struct *work)
{
	change_all_elevators(&req_queues.list, false);
}

static void set_sleep_sched_fn(struct work_struct *work)
{
	change_all_elevators(&req_queues.list, true);
}

int init_iosched_switcher(struct request_queue *q)
{
	struct req_queue_data *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	r->queue = q;

	spin_lock(&init_lock);
	list_add(&r->list, &req_queues.list);
	spin_unlock(&init_lock);

	return 0;
}

static int iosched_switcher_core_init(void)
{
	INIT_DELAYED_WORK(&restore_prev, restore_prev_fn);
	INIT_DELAYED_WORK(&sleep_sched, set_sleep_sched_fn);
	fb_register_client(&fb_notifier_callback_nb);

	return 0;
}
late_initcall(iosched_switcher_core_init);