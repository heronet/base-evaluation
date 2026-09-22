/* SPDX-License-Identifier: Apache-2.0
 * BASE Test 4 v1.0. Static-build comparison and separate stack profiling.
 * No sample presentation, enrollment, deletion, or device-driver changes.
 */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <stdint.h>

#if !defined(CONFIG_T4_BASELINE)
#include <zephyr/drivers/biometrics.h>
static const struct device *const bio = DEVICE_DT_GET(DT_ALIAS(biometrics));
#endif

static void result(const char *phase, int ret)
{
	printk("T4,V,%s,%s,%d\n", phase, ret == 0 ? "PASS" : "FAIL", ret);
}

#if defined(CONFIG_T4_ASYNC) || defined(CONFIG_T4_PROFILE)
K_SEM_DEFINE(terminal, 0, 1);
static unsigned int event_count, no_match_count, stopped_count;
static int terminal_status, event_error;
static k_tid_t callback_thread;

static void on_event(const struct device *dev, const struct biometric_event *e, void *arg)
{
	if (dev != bio || arg != NULL || k_is_in_isr()) { event_error = -EPROTO; }
	callback_thread = k_current_get();
	event_count++;
	if (e->type == BIOMETRIC_EVENT_NO_MATCH) {
		no_match_count++;
		if (stopped_count || e->status != -ETIMEDOUT) {
			event_error = e->status != 0 ? e->status : -EPROTO;
		}
	} else if (e->type == BIOMETRIC_EVENT_STOPPED) {
		stopped_count++;
		terminal_status = e->status;
		k_sem_give(&terminal);
	} else {
		/* An unexpected sample changes this no-sample workload. */
		event_error = e->status != 0 ? e->status : -EPROTO;
	}
}

static int callback_idle(void)
{
	int64_t end = k_uptime_get() + 2000;
	int ret;
	do {
		ret = biometric_callback_set(bio, on_event, NULL);
		if (ret != -EBUSY) { return ret; }
		k_sleep(K_MSEC(1));
	} while (k_uptime_get() < end);
	return -EBUSY;
}

static int async_once(bool continuous)
{
	int ret = callback_idle();
	if (ret != 0) { return ret; }
	/* Prior callback returned before its state or semaphore is reset. */
	event_count = 0; no_match_count = 0; stopped_count = 0;
	terminal_status = 0; event_error = 0;
	k_sem_reset(&terminal);
	ret = biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, continuous, K_SECONDS(1));
	if (ret != 0) { return ret; }
	if (continuous) {
		k_sleep(K_MSEC(300));
		ret = biometric_async_stop(bio);
		if (ret != 0) { return ret; }
	}
	ret = k_sem_take(&terminal, K_SECONDS(12));
	if (ret != 0) { return ret; }
	ret = callback_idle();
	if (ret != 0) { return ret; }
	if (event_error) { return event_error; }
	if (stopped_count != 1U || event_count != no_match_count + 1U) { return -EPROTO; }
	if (continuous) { return terminal_status == -ECANCELED ? 0 : -EPROTO; }
	return no_match_count == 1U && terminal_status == 0 ? 0 : -EPROTO;
}
#endif

#if defined(CONFIG_T4_SYNC) || defined(CONFIG_T4_PROFILE)
static int sync_once(void)
{
	struct biometric_match_result match;
	int ret = biometric_match(bio, BIOMETRIC_MATCH_IDENTIFY, 0, K_SECONDS(1), &match);
	return ret == -ETIMEDOUT ? 0 : (ret == 0 ? -EPROTO : ret);
}
#endif

#if defined(CONFIG_T4_PROFILE)
/* The observer scans main while main is blocked waiting for the snapshot.
 * The AI10 worker is idle after API completion/callback quiescence. There is no
 * concurrent enrollment or other application that changes those threads.
 */
K_SEM_DEFINE(snapshot_request, 0, 1);
K_SEM_DEFINE(snapshot_done, 0, 1);
static k_tid_t app_thread;
static const char *snapshot_phase;
static int snapshot_error;

static void stack_row(const char *role, k_tid_t thread)
{
	size_t unused = 0;
	int ret = thread != NULL ? k_thread_stack_space_get(thread, &unused) : -ENOENT;
	size_t usable = thread != NULL ? thread->stack_info.size : 0;
	if (ret == 0 && unused > usable) { ret = -ERANGE; }
	size_t used = ret == 0 ? usable - unused : 0;
	printk("T4,S,%s,%s,%d,%zu,%zu,%zu\n", snapshot_phase, role, ret, usable, unused, used);
	if (ret != 0) { snapshot_error = ret; }
}

static void observer(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		int ret = k_sem_take(&snapshot_request, K_FOREVER);
		if (ret != 0) { continue; }
		stack_row("app_main", app_thread);
		if (callback_thread != NULL) { stack_row("ai10_worker", callback_thread); }
		k_sem_give(&snapshot_done);
	}
}
K_THREAD_DEFINE(observer_tid, 2048, observer, NULL, NULL, NULL, 10, 0, 0);

static int snapshot(const char *phase)
{
	snapshot_phase = phase;
	snapshot_error = 0;
	k_sem_give(&snapshot_request);
	int ret = k_sem_take(&snapshot_done, K_SECONDS(3));
	return ret == 0 ? snapshot_error : ret;
}

static int profile(void)
{
	app_thread = k_current_get();
	printk("T4,M,stack_config,%d,%d\n", CONFIG_MAIN_STACK_SIZE, CONFIG_BIOMETRICS_AI10_STACK_SIZE);
	int ret = snapshot("before_operations");
	if (ret != 0) { return ret; }
	const char *const phases[] = { "sync_timeout", "async_timeout", "continuous_cancel" };
	for (unsigned int phase = 0; phase < ARRAY_SIZE(phases); ++phase) {
		for (unsigned int trial = 1; trial <= 3; ++trial) {
			ret = phase == 0 ? sync_once() : async_once(phase == 2);
			printk("T4,O,%s,%u,%s,%d\n", phases[phase], trial, ret == 0 ? "PASS" : "FAIL", ret);
			if (ret != 0) { return ret; }
		}
		ret = snapshot(phases[phase]);
		if (ret != 0) { return ret; }
	}
	return 0;
}
#endif

int main(void)
{
	int ret = 0;
	printk("T4,M,version,1,0\n");
#if defined(CONFIG_T4_BASELINE)
	printk("T4,M,variant,baseline\n");
#else
	if (!device_is_ready(bio)) { result("suite", -ENODEV); return 0; }
	struct biometric_capabilities caps;
	ret = biometric_get_capabilities(bio, &caps);
	if (ret != 0) { result("suite", ret); return 0; }
	printk("T4,M,caps,%u,%u\n", caps.supported_modalities, caps.async_operations);
#if defined(CONFIG_T4_IDLE)
	printk("T4,M,variant,idle\n");
#elif defined(CONFIG_T4_SYNC)
	printk("T4,M,variant,sync\n");
	ret = sync_once();
#elif defined(CONFIG_T4_ASYNC)
	printk("T4,M,variant,async\n");
	ret = (caps.async_operations & BIOMETRIC_ASYNC_IDENTIFY) ? async_once(false) : -ENOTSUP;
#elif defined(CONFIG_T4_PROFILE)
	printk("T4,M,variant,profile\n");
	printk("No face/palm samples. Profile starts in 5 seconds. No shell commands.\n");
	k_sleep(K_SECONDS(5));
	ret = (caps.async_operations & BIOMETRIC_ASYNC_IDENTIFY) ? profile() : -ENOTSUP;
#endif
#endif
	result("suite", ret);
	/* Keep stack-bearing threads alive on both success and error. */
	for (;;) { k_sleep(K_FOREVER); }
	return 0;
}
