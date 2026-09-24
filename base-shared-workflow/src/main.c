/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef BASE_BUILD_TAG
#define BASE_BUILD_TAG "manual"
#endif

#define LIMIT 4096
#define OP_MS 30000
#define EVENT_WAIT_MS 45000
#define IDLE_WAIT_MS 10000
#define CANCEL_DELAY_MS 5000

static const struct device *const dev = DEVICE_DT_GET(DT_ALIAS(biometrics));
static struct biometric_capabilities caps;
static uint16_t baseline[LIMIT], current[LIMIT];
static size_t baseline_n, current_n;
static const struct shell *out;
static unsigned int seq, run_number;
static bool poisoned; /* Require reboot after an unconfirmed idle state. */
static atomic_t event_loss;

struct queued_event {
	struct biometric_event event;
	int64_t ms;
};
K_MSGQ_DEFINE(events, sizeof(struct queued_event), 16, 8);

/* Only the command thread prints records or calls device APIs. */
#define REC(kind, fmt, ...) \
	shell_print(out, "BASE,%u,%lld," kind "," fmt, ++seq, \
		    (long long)k_uptime_get(), ##__VA_ARGS__)

static void on_event(const struct device *device, const struct biometric_event *event, void *user)
{
	struct queued_event item = {.event = *event, .ms = k_uptime_get()};
	ARG_UNUSED(device);
	ARG_UNUSED(user);
	if (k_msgq_put(&events, &item, K_NO_WAIT) != 0) {
		atomic_inc(&event_loss);
	}
}

static int compare_id(const void *a, const void *b)
{
	return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static bool contains(const uint16_t *ids, size_t n, uint16_t id)
{
	return bsearch(&id, ids, n, sizeof(*ids), compare_id) != NULL;
}

static int inventory(const char *phase)
{
	current_n = 0;
	int ret = biometric_template_list(dev, current, caps.max_templates, &current_n);
	REC("inventory", "%s,%d,%u", phase, ret, (unsigned int)current_n);
	if (ret != 0) {
		/* Error output is never treated as a partial or empty inventory. */
		current_n = 0;
		return ret;
	}
	if (current_n > caps.max_templates) {
		return -EOVERFLOW;
	}
	qsort(current, current_n, sizeof(*current), compare_id);
	for (size_t i = 0; i < current_n; ++i) {
		if (!current[i] || current[i] > caps.max_templates ||
		    (i && current[i] == current[i - 1])) {
			return -EBADMSG;
		}
		REC("id", "%s,%u", phase, current[i]);
	}
	return 0;
}

/* This checks ID-set restoration, not template contents or recognition accuracy. */
static int restore_inventory(const char *phase)
{
	int ret = inventory(phase);
	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i < baseline_n; ++i) {
		if (!contains(current, current_n, baseline[i])) {
			REC("missing_original", "%u", baseline[i]);
			return -EIO;
		}
	}
	for (size_t i = 0; i < current_n; ++i) {
		if (!contains(baseline, baseline_n, current[i])) {
			/* Sole device owner; only IDs absent before this run are deleted. */
			ret = biometric_template_delete(dev, current[i]);
			REC("delete_new", "%u,%d", current[i], ret);
			if (ret != 0) {
				return ret;
			}
		}
	}
	ret = inventory("restored");
	if (ret == 0 && (current_n != baseline_n ||
	    memcmp(current, baseline, baseline_n * sizeof(*baseline)) != 0)) {
		ret = -EIO;
	}
	REC("restoration", "%s,%d", phase, ret);
	return ret;
}

/* Idle-only registration is an observable barrier after STOPPED delivery.
 * Callback state is static and is not reset until this succeeds.
 */
static int wait_idle(void)
{
	int64_t until = k_uptime_get() + IDLE_WAIT_MS;
	int ret;
	do {
		ret = biometric_callback_set(dev, on_event, NULL);
		if (ret != -EBUSY) {
			break;
		}
		k_msleep(1);
	} while (k_uptime_get() < until);
	REC("idle", "%d", ret);
	if (ret != 0) {
		poisoned = true;
	}
	return ret;
}

struct outcome {
	unsigned int enrolled, stopped, errors;
	int terminal;
	uint16_t id;
	uint32_t modality;
};

static int consume_events(struct outcome *result)
{
	struct queued_event item;
	int64_t until = k_uptime_get() + EVENT_WAIT_MS;
	int ret = 0;

	memset(result, 0, sizeof(*result));
	for (;;) {
		int64_t left = until - k_uptime_get();
		if (left <= 0 || k_msgq_get(&events, &item, K_MSEC(left)) != 0) {
			poisoned = true;
			return -ETIMEDOUT;
		}
		const struct biometric_event *e = &item.event;
		REC("event", "%d,%d,%lld", e->type, e->status, (long long)item.ms);
		if (e->type == BIOMETRIC_EVENT_ENROLL_COMPLETE) {
			result->enrolled++;
			result->id = e->enrollment.template_id;
			result->modality = e->enrollment.modality;
			REC("enrolled", "%u,%u", result->id, (unsigned int)result->modality);
			if (e->status != 0) {
				ret = -EBADMSG;
			}
		} else if (e->type == BIOMETRIC_EVENT_ERROR) {
			result->errors++;
		} else if (e->type == BIOMETRIC_EVENT_STOPPED) {
			result->stopped++;
			result->terminal = e->status;
			break;
		} else {
			ret = -EBADMSG; /* Only enrollment is asynchronous in this app. */
		}
	}
	int idle_ret = wait_idle();
	if (idle_ret != 0) {
		return idle_ret;
	}
	/* Once idle is confirmed, no callbacks from this operation may remain. */
	while (k_msgq_get(&events, &item, K_NO_WAIT) == 0) {
		REC("extra_event", "%d,%d", item.event.type, item.event.status);
		ret = -EBADMSG;
	}
	if (atomic_get(&event_loss) != 0) {
		REC("event_loss", "%ld", (long)atomic_get(&event_loss));
		ret = -EOVERFLOW;
	}
	return ret;
}

static int abort_staged(void)
{
	int ret = biometric_enroll_abort(dev);
	REC("abort", "%d", ret);
	if (ret != 0 && ret != -EALREADY) {
		poisoned = true;
		return ret;
	}
	return 0;
}

static int capture(unsigned int expected)
{
	struct biometric_capture_result result = {0};
	shell_print(out, "Present the same finger; lift it as soon as the sensor accepts it.");
	int ret = biometric_enroll_capture(dev, K_MSEC(OP_MS), &result);
	REC("capture", "%d,%u,%u,%u", ret, result.samples_captured,
	    result.samples_required, result.quality);
	if (ret == 0 && (result.samples_captured != expected ||
	    result.samples_required != caps.enrollment_samples_required)) {
		ret = -EBADMSG;
	}
	if (ret == 0) {
		shell_print(out, "Lift finger. Next step in 3 seconds.");
		k_msleep(3000);
	}
	return ret;
}

static int staged(uint16_t id, bool interrupt)
{
	int ret = biometric_enroll_start(dev, id);
	REC("start_staged", "%u,%d", id, ret);
	if (ret != 0) {
		/* A rejected or failed start does not establish an idle host session. */
		poisoned = true;
		return ret;
	}
	unsigned int count = interrupt ? 1U : caps.enrollment_samples_required;
	for (unsigned int i = 1; i <= count; ++i) {
		ret = capture(i);
		if (ret != 0) {
			abort_staged();
			return ret;
		}
	}
	if (interrupt) {
		return abort_staged();
	}
	ret = biometric_enroll_finalize(dev);
	REC("finalize", "%d", ret);
	if (ret != 0) {
		abort_staged();
	}
	return ret;
}

static int managed(bool interrupt, uint32_t expected_modality, uint16_t *id)
{
	struct outcome result;
	int ret;

	k_msgq_purge(&events); /* Previous operation is already idle. */
	atomic_clear(&event_loss);
	ret = biometric_enroll_async(dev, BIOMETRIC_ID_AUTO, K_MSEC(OP_MS));
	REC("start_managed", "%d", ret);
	if (ret != 0) {
		return ret;
	}
	int stop_ret = 0;
	if (interrupt) {
		k_msleep(CANCEL_DELAY_MS);
		stop_ret = biometric_async_stop(dev);
		REC("stop", "%d", stop_ret);
	}
	ret = consume_events(&result);
	if (ret != 0) {
		return ret;
	}
	if (interrupt) {
		/* A completion winning the race is recorded, not called cancellation. */
		if (stop_ret != 0 || result.enrolled != 0 || result.errors != 0 ||
		    result.terminal != -ECANCELED) {
			return -EIO;
		}
	} else if (result.enrolled != 1 || result.errors != 0 ||
		   result.terminal != 0 || result.modality != expected_modality ||
		   !result.id || result.id > caps.max_templates) {
		return -EIO;
	}
	*id = result.id;
	return 0;
}

static uint16_t free_id(void)
{
	for (unsigned int id = 1; id <= caps.max_templates; ++id) {
		if (!contains(baseline, baseline_n, id)) {
			return id;
		}
	}
	return 0;
}

static int workflow(uint32_t modality)
{
	bool is_staged = caps.enrollment_samples_required > 0;
	uint16_t id = is_staged ? free_id() : BIOMETRIC_ID_AUTO;
	int ret;

	if (is_staged && (caps.enrollment_samples_required < 2 || id == 0)) {
		return -ENOTSUP;
	}
	if (!is_staged) {
		if (!(caps.async_operations & BIOMETRIC_ASYNC_ENROLL)) {
			return -ENOTSUP;
		}
		ret = wait_idle();
		if (ret != 0) {
			return ret;
		}
	}

	REC("phase", "interruption");
	if (is_staged) {
		shell_print(out, "Recovery test: one capture, then abort BEFORE the remaining captures.");
	} else {
		shell_print(out, "Recovery test: KEEP FACE AND PALM OUT OF VIEW. Cancellation in 5 seconds.");
		k_msleep(5000);
	}
	ret = is_staged ? staged(id, true) : managed(true, modality, &id);
	REC("recovery_operation", "%d", ret);
	if (ret != 0 || poisoned) {
		goto recover_failure;
	}
	/* Read before cleanup to expose any persistent change after interruption. */
	ret = inventory("after_interrupt");
	if (ret != 0) {
		return ret;
	}
	REC("interrupted_inventory_equal", "%d",
	    current_n == baseline_n &&
	    memcmp(current, baseline, baseline_n * sizeof(*baseline)) == 0);
	ret = restore_inventory("recovery_cleanup");
	if (ret != 0) {
		return ret;
	}
	REC("recovery", "0");

	REC("phase", "successful_workflow");
	shell_print(out, "Enrollment begins in 5 seconds. Use a finger/face/palm NOT already enrolled.");
	k_msleep(5000);
	if (is_staged) {
		id = free_id();
	}
	ret = is_staged ? staged(id, false) : managed(false, modality, &id);
	REC("enrollment", "%d,%u", ret, id);
	if (ret != 0 || poisoned) {
		goto recover_failure;
	}
	ret = inventory("after_enrollment");
	if (ret != 0) {
		return ret;
	}
	if (contains(baseline, baseline_n, id) || !contains(current, current_n, id) ||
	    current_n != baseline_n + 1) {
		ret = -EIO;
		goto recover_failure;
	}
	for (size_t i = 0; i < baseline_n; ++i) {
		if (!contains(current, current_n, baseline[i])) {
			return -EIO;
		}
	}
	struct biometric_match_result match = {0};
	shell_print(out, "Identification begins in 3 seconds. Present the newly enrolled sample.");
	k_msleep(3000);
	ret = biometric_match(dev, BIOMETRIC_MATCH_IDENTIFY, 0, K_MSEC(OP_MS), &match);
	REC("identify", "%d,%u,%u,%ld", ret, match.template_id,
	    (unsigned int)match.modality, (long)match.confidence);
	if (ret == 0 && (match.template_id != id || match.modality != modality)) {
		ret = -EIO;
	}
	if (ret != 0) {
		goto recover_failure;
	}
	return restore_inventory("final_cleanup");

recover_failure:
	/* Never attempt database control unless host-side inactivity is known. */
	if (!poisoned) {
		int cleanup_ret = restore_inventory("failed_run_cleanup");
		REC("failure_cleanup", "%d", cleanup_ret);
	}
	return ret != 0 ? ret : -EIO;
}

static int cmd_run(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	out = shell;
	if (poisoned) {
		shell_error(shell, "Idle state was not confirmed. Reboot before another run.");
		return -EBUSY;
	}
	uint32_t modality;
	if (strcmp(argv[1], "fingerprint") == 0) {
		modality = BIOMETRIC_MODALITY_FINGERPRINT;
	} else if (strcmp(argv[1], "face") == 0) {
		modality = BIOMETRIC_MODALITY_FACE;
	} else if (strcmp(argv[1], "palm") == 0) {
		modality = BIOMETRIC_MODALITY_PALM;
	} else {
		shell_error(shell, "Use: base run fingerprint|face|palm");
		return -EINVAL;
	}
	seq = 0;
	REC("begin", "%s,%u,%u", BASE_BUILD_TAG, ++run_number, (unsigned int)modality);
	int ret = device_is_ready(dev) ? biometric_get_capabilities(dev, &caps) : -ENODEV;
	if (ret != 0) {
		goto end;
	}
	REC("caps", "%u,%u,%u,%u,%u", (unsigned int)caps.supported_modalities,
	    caps.max_templates, caps.enrollment_samples_required,
	    (unsigned int)caps.async_operations, caps.storage_modes);
	if (!(caps.supported_modalities & modality) ||
	    !(caps.storage_modes & BIOMETRIC_STORAGE_DEVICE) || !caps.max_templates ||
	    caps.max_templates > LIMIT || caps.max_templates > CONFIG_BASE_INVENTORY_COVERAGE) {
		ret = -ENOTSUP;
		goto end;
	}
	ret = inventory("baseline");
	if (ret != 0) {
		REC("blocked", "baseline_inventory,%d", ret);
		goto end;
	}
	baseline_n = current_n;
	memcpy(baseline, current, baseline_n * sizeof(*baseline));
	ret = workflow(modality);
end:
	REC("end", "%s,%d", ret == 0 ? "PASS" : "FAIL", ret);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(base_commands,
	SHELL_CMD_ARG(run, NULL, "Run recovery, enrollment, identify and cleanup", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(base, &base_commands, "Shared BASE workflow", NULL);

int main(void)
{
	return 0;
}
