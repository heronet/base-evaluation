/* SPDX-License-Identifier: Apache-2.0
 * BASE GT5 Test 4 v1.0. Static-build comparison and separate stack profiling.
 * No sample presentation, enrollment, deletion, or device-driver changes.
 */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdint.h>

#if !defined(CONFIG_T4_BASELINE)
#include <zephyr/drivers/biometrics.h>
BUILD_ASSERT(DT_PROP(DT_ALIAS(biometrics), template_size) == 498);
BUILD_ASSERT(DT_PROP(DT_ALIAS(biometrics), max_templates) == 200);
BUILD_ASSERT(DT_PROP(DT_BUS(DT_ALIAS(biometrics)), current_speed) == 9600);
static const struct device *const bio = DEVICE_DT_GET(DT_ALIAS(biometrics));
#endif

static void result(const char *phase, int ret)
{
	printk("G4,V,%s,%s,%d\n", phase, ret == 0 ? "PASS" : "FAIL", ret);
}

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
 * GT5 has no private worker thread. This scans the application stack used by
 * synchronous calls; it does not scan the interrupt stack or observer stack.
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
	printk("G4,S,%s,%s,%d,%zu,%zu,%zu\n", snapshot_phase, role, ret, usable, unused, used);
	if (ret != 0) { snapshot_error = ret; }
}

static void observer(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		int ret = k_sem_take(&snapshot_request, K_FOREVER);
		if (ret != 0) { continue; }
		stack_row("app_main", app_thread);
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
	printk("G4,M,stack_config,%d,%d\n", CONFIG_MAIN_STACK_SIZE, 0);
	printk("G4,M,heap_pool_config,%d\n", CONFIG_HEAP_MEM_POOL_SIZE);
	int ret = snapshot("before_operations");
	if (ret != 0) { return ret; }
	for (unsigned int trial = 1; trial <= 3; ++trial) {
		ret = sync_once();
		printk("G4,O,sync_timeout,%u,%s,%d\n", trial, ret == 0 ? "PASS" : "FAIL", ret);
		if (ret != 0) { return ret; }
	}
	return snapshot("sync_timeout");
}
#endif

int main(void)
{
	int ret = 0;
	printk("G4,M,version,1,0\n");
#if defined(CONFIG_T4_BASELINE)
	printk("G4,M,variant,baseline\n");
#else
	if (!device_is_ready(bio)) { result("suite", -ENODEV); return 0; }
	struct biometric_capabilities caps;
	ret = biometric_get_capabilities(bio, &caps);
	if (ret != 0) { result("suite", ret); return 0; }
	printk("G4,M,caps,%u,%u\n", caps.supported_modalities, caps.async_operations);
#if defined(CONFIG_T4_IDLE)
	printk("G4,M,variant,idle\n");
#elif defined(CONFIG_T4_SYNC)
	printk("G4,M,variant,sync\n");
	ret = sync_once();
#elif defined(CONFIG_T4_PROFILE)
	printk("G4,M,variant,profile\n");
	printk("Keep your finger OFF the sensor. Profile starts in 5 seconds. No shell commands.\n");
	k_sleep(K_SECONDS(5));
	ret = (caps.supported_modalities == BIOMETRIC_MODALITY_FINGERPRINT &&
	       caps.async_operations == 0) ? profile() : -ENOTSUP;
#endif
#endif
	result("suite", ret);
	/* Keep stack-bearing threads alive on both success and error. */
	for (;;) { k_sleep(K_FOREVER); }
	return 0;
}
