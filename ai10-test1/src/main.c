/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Siratul Islam
 *
 * BASE Test 1: functional integration and application portability.
 *
 * This is an interactive functional test, not a timing benchmark.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BIO_NODE DT_ALIAS(biometrics)

#if !DT_NODE_EXISTS(BIO_NODE)
#error "Define a devicetree alias named biometrics"
#endif

#define INVENTORY_CAPACITY 3000
#define EVENT_CAPACITY     32
#define STAGED_TEST_SLOTS  20

#define CAPTURE_TIMEOUT K_SECONDS(15)
#define MATCH_TIMEOUT   K_SECONDS(15)
#define ENROLL_TIMEOUT  K_SECONDS(30)

static const struct device *const bio = DEVICE_DT_GET(BIO_NODE);

static uint16_t before_ids[INVENTORY_CAPACITY];
static uint16_t after_ids[INVENTORY_CAPACITY];

K_MUTEX_DEFINE(test_lock);

/*
 * Set after an unresolved operation or uncertain database state.
 * Prevents blindly continuing after a failed enrollment or cleanup.
 */
static bool halted;

struct recorded_event {
	struct biometric_event event;
	uint32_t callback_ms;
};

K_MSGQ_DEFINE(event_queue, sizeof(struct recorded_event), EVENT_CAPACITY, 4);

static atomic_t callback_stops;
static atomic_t dropped_events;

struct trial {
	const struct shell *sh;
	unsigned int run;
	const char *sample;
	uint32_t expected_modality;
	unsigned int failures;
};

static uint32_t parse_modality(const char *name)
{
	if (strcmp(name, "fingerprint") == 0) {
		return BIOMETRIC_MODALITY_FINGERPRINT;
	}
	if (strcmp(name, "face") == 0) {
		return BIOMETRIC_MODALITY_FACE;
	}
	if (strcmp(name, "palm") == 0) {
		return BIOMETRIC_MODALITY_PALM;
	}
	return 0;
}

/*
 * CSV columns:
 * record,run,case,step,outcome,rc,id,modality,value1,value2,uptime_ms
 *
 * For event rows:
 *   value1 = callback timestamp in milliseconds
 *   value2 = event type
 *
 * Timestamps are diagnostic only; they are not precision benchmarks.
 */
static void emit(struct trial *t, const char *step, const char *outcome, int rc, uint16_t id,
		 uint32_t modality, unsigned int value1, unsigned int value2)
{
	shell_print(t->sh, "T1,%u,%s,%s,%s,%d,%u,%u,%u,%u,%u", t->run, t->sample, step, outcome, rc,
		    (unsigned int)id, (unsigned int)modality, value1, value2,
		    (unsigned int)k_uptime_get_32());
}

static bool check(struct trial *t, const char *step, bool passed, int rc, uint16_t id,
		  uint32_t modality, unsigned int value1, unsigned int value2)
{
	if (!passed) {
		t->failures++;
	}

	emit(t, step, passed ? "PASS" : "FAIL", rc, id, modality, value1, value2);
	return passed;
}

static void report_caps(struct trial *t, const struct biometric_capabilities *caps)
{
	emit(t, "caps_modalities_async", "INFO", 0, 0, 0, (unsigned int)caps->supported_modalities,
	     (unsigned int)caps->async_operations);

	emit(t, "caps_samples_capacity", "INFO", 0, 0, 0,
	     (unsigned int)caps->enrollment_samples_required, (unsigned int)caps->max_templates);

	emit(t, "caps_storage_template_size", "INFO", 0, 0, 0, (unsigned int)caps->storage_modes,
	     (unsigned int)caps->template_size);
}

static bool contains(const uint16_t *ids, size_t count, uint16_t id)
{
	for (size_t i = 0; i < count; i++) {
		if (ids[i] == id) {
			return true;
		}
	}
	return false;
}

static bool same_inventory(const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
	if (na != nb) {
		return false;
	}

	for (size_t i = 0; i < na; i++) {
		if (!contains(b, nb, a[i])) {
			return false;
		}
	}
	return true;
}

static int snapshot(uint16_t *ids, size_t *count, uint16_t max_id)
{
	int ret;

	*count = 0;
	ret = biometric_template_list(bio, ids, INVENTORY_CAPACITY, count);
	if (ret != 0) {
		return ret;
	}

	if (*count > INVENTORY_CAPACITY) {
		return -EOVERFLOW;
	}

	for (size_t i = 0; i < *count; i++) {
		if (ids[i] == 0 || ids[i] > max_id) {
			return -EBADMSG;
		}
		for (size_t j = 0; j < i; j++) {
			if (ids[i] == ids[j]) {
				return -EBADMSG;
			}
		}
	}

	return 0;
}

static void print_inventory(struct trial *t, const char *step, const uint16_t *ids, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		emit(t, step, "INFO", 0, ids[i], 0, (unsigned int)i, (unsigned int)count);
	}
}

static uint16_t choose_staged_id(size_t count, uint16_t capacity)
{
	unsigned int limit = MIN((unsigned int)capacity, STAGED_TEST_SLOTS);

	for (unsigned int id = 1; id <= limit; id++) {
		if (!contains(before_ids, count, (uint16_t)id)) {
			return (uint16_t)id;
		}
	}
	return 0;
}

static void on_event(const struct device *dev, const struct biometric_event *event, void *user_data)
{
	struct recorded_event record = {
		.event = *event,
		.callback_ms = k_uptime_get_32(),
	};

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (k_msgq_put(&event_queue, &record, K_NO_WAIT) != 0) {
		atomic_inc(&dropped_events);
	}

	/*
	 * Separate notification prevents a lost queue entry from hiding
	 * the fact that a terminal callback was invoked.
	 */
	if (event->type == BIOMETRIC_EVENT_STOPPED) {
		atomic_inc(&callback_stops);
	}
}

/*
 * Callback replacement is permitted only while idle in the supplied
 * contract. Successful unregistration therefore establishes that the
 * terminal callback has returned and the device has been released.
 */
static int detach_callback(void)
{
	int64_t deadline = k_uptime_get() + 2000;

	do {
		int ret = biometric_callback_set(bio, NULL, NULL);

		if (ret != -EBUSY) {
			return ret;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static int enroll_staged(struct trial *t, const struct biometric_capabilities *caps, uint16_t id)
{
	int ret = biometric_enroll_start(bio, id);

	if (!check(t, "staged_start", ret == 0, ret, id, 0, 0, 0)) {
		return ret;
	}

	for (unsigned int i = 0; i < caps->enrollment_samples_required; i++) {
		struct biometric_capture_result result = {0};

		shell_print(t->sh, "Present sample %u/%u.", i + 1,
			    (unsigned int)caps->enrollment_samples_required);

		ret = biometric_enroll_capture(bio, CAPTURE_TIMEOUT, &result);

		bool good = ret == 0 && result.samples_captured == i + 1 &&
			    result.samples_required == caps->enrollment_samples_required;

		check(t, "capture", good, ret, id, 0, (unsigned int)result.samples_captured,
		      (unsigned int)result.samples_required);

		if (!good) {
			int abort_ret = biometric_enroll_abort(bio);

			emit(t, "abort_after_failure", "INFO", abort_ret, id, 0, 0, 0);
			return ret != 0 ? ret : -EPROTO;
		}

		if (i + 1 < caps->enrollment_samples_required) {
			shell_print(t->sh, "Remove the finger; prepare to present it again.");
			k_sleep(K_SECONDS(2));
		}
	}

	ret = biometric_enroll_finalize(bio);
	check(t, "staged_finalize", ret == 0, ret, id, 0, 0, 0);

	if (ret != 0) {
		int abort_ret = biometric_enroll_abort(bio);

		emit(t, "abort_after_failure", "INFO", abort_ret, id, 0, 0, 0);
	}

	return ret;
}

static int enroll_async(struct trial *t, uint16_t *id, uint32_t *modality)
{
	int ret = biometric_callback_set(bio, on_event, NULL);

	if (!check(t, "callback_register", ret == 0, ret, 0, 0, 0, 0)) {
		return ret;
	}

	/* Registration succeeded while idle. Prepare before starting. */
	k_msgq_purge(&event_queue);
	atomic_clear(&callback_stops);
	atomic_clear(&dropped_events);

	shell_print(t->sh, "Present the %s sample within 30 seconds.", t->sample);

	int start_ret = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, ENROLL_TIMEOUT);

	bool accepted = start_ret == 0;

	check(t, "async_accept", accepted, start_ret, 0, 0, 0, 0);

	if (accepted) {
		/*
		 * Test-runner watchdog, not a claimed API timing bound.
		 * Do not call an indefinitely waiting stop from here.
		 */
		int64_t deadline = k_uptime_get() + 60000;

		while (atomic_get(&callback_stops) == 0 && k_uptime_get() < deadline) {
			k_msleep(10);
		}
	}

	int release_ret;

	if (accepted && atomic_get(&callback_stops) == 0) {
		release_ret = -ETIMEDOUT;
		check(t, "terminal_watchdog", false, release_ret, 0, 0, 0, 0);
		halted = true;
	} else {
		release_ret = detach_callback();
		check(t, "callback_quiescence", release_ret == 0, release_ret, 0, 0, 0, 0);

		if (release_ret != 0) {
			halted = true;
		}
	}

	unsigned int completions = 0;
	unsigned int stops = 0;
	unsigned int errors = 0;
	unsigned int unexpected = 0;
	int stop_status = 0;
	bool terminal_seen = false;

	/* Bounded drain also works when the watchdog has expired. */
	for (unsigned int i = 0; i < EVENT_CAPACITY; i++) {
		struct recorded_event record;

		if (k_msgq_get(&event_queue, &record, K_NO_WAIT) != 0) {
			break;
		}

		const struct biometric_event *event = &record.event;
		uint16_t event_id = 0;
		uint32_t event_modality = 0;
		const char *name;

		if (terminal_seen) {
			unexpected++;
		}

		switch (event->type) {
		case BIOMETRIC_EVENT_ENROLL_COMPLETE:
			name = "event_enroll_complete";
			completions++;
			event_id = event->enrollment.template_id;
			event_modality = event->enrollment.modality;
			*id = event_id;
			*modality = event_modality;
			break;
		case BIOMETRIC_EVENT_ERROR:
			name = "event_error";
			errors++;
			break;
		case BIOMETRIC_EVENT_STOPPED:
			name = "event_stopped";
			stops++;
			stop_status = event->status;
			terminal_seen = true;
			break;
		default:
			name = "event_unexpected";
			unexpected++;
			break;
		}

		emit(t, name, "INFO", event->status, event_id, event_modality,
		     (unsigned int)record.callback_ms, (unsigned int)event->type);
	}

	unsigned int dropped = (unsigned int)atomic_get(&dropped_events);

	check(t, "event_recording", dropped == 0, dropped == 0 ? 0 : -ENOBUFS, 0, 0, dropped, 0);

	if (!accepted) {
		check(t, "rejected_request_events",
		      completions == 0 && stops == 0 && errors == 0 && unexpected == 0, 0, 0, 0,
		      completions, stops);
		return start_ret;
	}

	bool good = release_ret == 0 && completions == 1 && stops == 1 && errors == 0 &&
		    unexpected == 0 && stop_status == 0 && dropped == 0;

	check(t, "async_enrollment_trace", good, stop_status, *id, *modality, completions, stops);

	if (release_ret != 0) {
		return release_ret;
	}

	return good ? 0 : -EPROTO;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct biometric_capabilities caps;
	struct trial t = {
		.sh = sh,
		.run = 0,
		.sample = "info",
	};
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!device_is_ready(bio)) {
		shell_error(sh, "Biometrics device is not ready.");
		return -ENODEV;
	}

	ret = biometric_get_capabilities(bio, &caps);
	if (ret == 0) {
		shell_print(sh, "Device: %s", bio->name);
		report_caps(&t, &caps);
	}

	return ret;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long run;
	bool expect_verify;
	struct biometric_capabilities caps;
	size_t before_count = 0;
	size_t after_count = 0;
	uint16_t enrolled_id = 0;
	uint32_t enrollment_modality = 0;
	bool staged;
	int ret;

	ARG_UNUSED(argc);

	errno = 0;
	run = strtoul(argv[1], &end, 10);

	if (errno != 0 || argv[1][0] < '0' || argv[1][0] > '9' || *end != '\0' || run == 0 ||
	    run > 1000000) {
		shell_error(sh, "Run number must be 1..1000000.");
		return -EINVAL;
	}

	uint32_t expected = parse_modality(argv[2]);

	if (expected == 0 || (strcmp(argv[3], "yes") != 0 && strcmp(argv[3], "no") != 0)) {
		shell_error(sh, "Usage: t1 run <number> <fingerprint|face|palm> "
				"<verification-supported:yes|no>");
		return -EINVAL;
	}

	expect_verify = strcmp(argv[3], "yes") == 0;

	if (k_mutex_lock(&test_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	struct trial t = {
		.sh = sh,
		.run = (unsigned int)run,
		.sample = argv[2],
		.expected_modality = expected,
	};

	emit(&t, "begin", "INFO", 0, 0, expected, expect_verify ? 1U : 0U, 0);

	if (!check(&t, "runner_available", !halted, halted ? -EBUSY : 0, 0, 0, 0, 0)) {
		goto done;
	}

	if (!check(&t, "device_ready", device_is_ready(bio), device_is_ready(bio) ? 0 : -ENODEV, 0,
		   0, 0, 0)) {
		goto done;
	}

	ret = biometric_get_capabilities(bio, &caps);
	if (!check(&t, "capabilities", ret == 0, ret, 0, 0, 0, 0)) {
		goto done;
	}

	report_caps(&t, &caps);

	if (!check(&t, "inventory_capacity",
		   caps.max_templates > 0 && caps.max_templates <= INVENTORY_CAPACITY, 0, 0, 0,
		   caps.max_templates, INVENTORY_CAPACITY)) {
		goto done;
	}

	if (!check(&t, "requested_modality", (caps.supported_modalities & expected) != 0, 0, 0,
		   expected, caps.supported_modalities, 0)) {
		goto done;
	}

	staged = caps.enrollment_samples_required > 0;

	if (!check(&t, "enrollment_available",
		   staged || (caps.async_operations & BIOMETRIC_ASYNC_ENROLL) != 0, 0, 0, 0, 0,
		   0)) {
		goto done;
	}

	ret = snapshot(before_ids, &before_count, caps.max_templates);
	if (!check(&t, "list_before", ret == 0, ret, 0, 0, (unsigned int)before_count, 0)) {
		goto done;
	}

	print_inventory(&t, "inventory_before", before_ids, before_count);

	if (staged) {
		enrolled_id = choose_staged_id(before_count, caps.max_templates);
		if (!check(&t, "free_test_slot", enrolled_id != 0, enrolled_id != 0 ? 0 : -ENOSPC,
			   enrolled_id, 0, 0, 0)) {
			shell_error(sh, "No unused test ID within slots 1..20.");
			goto done;
		}
	}

	/*
	 * Expectations for the supplied driver set.
	 * Capability selection is independent of vendor/model names.
	 */
	if (staged && (caps.async_operations & BIOMETRIC_ASYNC_ENROLL) == 0) {
		ret = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, ENROLL_TIMEOUT);
		if (!check(&t, "unsupported_async_enroll", ret == -ENOSYS, ret, 0, 0, 0, 0)) {
			halted = true;
			goto done;
		}
	} else if (!staged) {
		ret = biometric_enroll_start(bio, 1);
		if (!check(&t, "unsupported_staged_enroll", ret == -ENOSYS, ret, 0, 0, 0, 0)) {
			halted = true;
			goto done;
		}
	}

	shell_print(sh, "Run %u: enroll %s.", t.run, t.sample);

	if (staged) {
		ret = enroll_staged(&t, &caps, enrolled_id);
	} else {
		ret = enroll_async(&t, &enrolled_id, &enrollment_modality);
	}

	if (!check(&t, "enrollment", ret == 0, ret, enrolled_id, enrollment_modality, 0, 0)) {
		/*
		 * Enrollment failure may still leave a device-stored record.
		 * Preserve evidence and stop rather than guessing ownership.
		 */
		halted = true;
		goto done;
	}

	if (!check(&t, "enrolled_id_range", enrolled_id > 0 && enrolled_id <= caps.max_templates, 0,
		   enrolled_id, enrollment_modality, 0, 0)) {
		halted = true;
		goto done;
	}

	if (!staged) {
		check(&t, "enrollment_modality", enrollment_modality == expected, 0, enrolled_id,
		      enrollment_modality, expected, 0);
	}

	ret = snapshot(after_ids, &after_count, caps.max_templates);
	if (!check(&t, "list_after_enroll", ret == 0, ret, enrolled_id, 0,
		   (unsigned int)after_count, 0)) {
		halted = true;
		goto done;
	}

	print_inventory(&t, "inventory_after_enroll", after_ids, after_count);

	bool added_only_test_id = !contains(before_ids, before_count, enrolled_id) &&
				  contains(after_ids, after_count, enrolled_id) &&
				  after_count == before_count + 1;

	for (size_t i = 0; i < before_count; i++) {
		if (!contains(after_ids, after_count, before_ids[i])) {
			added_only_test_id = false;
		}
	}

	if (!check(&t, "new_record_only", added_only_test_id, 0, enrolled_id, 0,
		   (unsigned int)before_count, (unsigned int)after_count)) {
		halted = true;
		goto done;
	}

	/*
	 * The ID is now verified as a newly added test record.
	 * Subsequent match failures do not prevent its cleanup.
	 */
	shell_print(sh, "Remove the sample. Identification starts in 2 seconds.");
	k_sleep(K_SECONDS(2));
	shell_print(sh, "Present the SAME %s sample.", t.sample);

	struct biometric_match_result result = {0};

	ret = biometric_match(bio, BIOMETRIC_MATCH_IDENTIFY, 0, MATCH_TIMEOUT, &result);

	check(&t, "identify",
	      ret == 0 && result.template_id == enrolled_id && result.modality == expected, ret,
	      result.template_id, result.modality, enrolled_id, expected);

	memset(&result, 0, sizeof(result));

	if (expect_verify) {
		shell_print(sh, "Verification starts in 2 seconds; use the same sample.");
		k_sleep(K_SECONDS(2));
	}

	ret = biometric_match(bio, BIOMETRIC_MATCH_VERIFY, enrolled_id, MATCH_TIMEOUT, &result);

	if (expect_verify) {
		check(&t, "verify",
		      ret == 0 && result.template_id == enrolled_id && result.modality == expected,
		      ret, result.template_id, result.modality, enrolled_id, expected);
	} else {
		check(&t, "unsupported_verify", ret == -ENOTSUP, ret, enrolled_id, 0, 0, 0);
	}

	ret = biometric_template_delete(bio, enrolled_id);
	check(&t, "delete_test_record", ret == 0, ret, enrolled_id, 0, 0, 0);

	if (ret != 0) {
		halted = true;
		goto done;
	}

	ret = snapshot(after_ids, &after_count, caps.max_templates);
	if (!check(&t, "list_after_delete", ret == 0, ret, enrolled_id, 0,
		   (unsigned int)after_count, 0)) {
		halted = true;
		goto done;
	}

	print_inventory(&t, "inventory_after_delete", after_ids, after_count);

	bool restored = !contains(after_ids, after_count, enrolled_id) &&
			same_inventory(before_ids, before_count, after_ids, after_count);

	check(&t, "inventory_restored", restored, 0, enrolled_id, 0, (unsigned int)before_count,
	      (unsigned int)after_count);

	if (!restored) {
		halted = true;
	}

done:
	emit(&t, "summary", t.failures == 0 ? "PASS" : "FAIL", t.failures == 0 ? 0 : -EIO,
	     enrolled_id, expected, t.failures, halted ? 1U : 0U);

	if (halted) {
		shell_error(sh, "Further trials halted. Save this log and inspect "
				"device/template state before resetting and retrying.");
	}

	k_mutex_unlock(&test_lock);
	return t.failures == 0 ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t1_commands, SHELL_CMD(info, NULL, "Show capabilities.", cmd_info),
			       SHELL_CMD_ARG(run, NULL,
					     "Run: <number> <fingerprint|face|palm> "
					     "<verification-supported:yes|no>",
					     cmd_run, 4, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(t1, &t1_commands, "BASE functional portability evaluation", NULL);

int main(void)
{
	printk("BASE Test 1 ready. Use 't1 info' first.\n");
	return 0;
}
