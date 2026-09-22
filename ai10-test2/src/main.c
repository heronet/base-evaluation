/*
 * SPDX-License-Identifier: Apache-2.0
 * BASE Test 2A: AI10 hardware lifecycle checks, harness version 1.0.
 * Run with no face or palm presented. Does not delete any template.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"

#define BIO_NODE DT_ALIAS(biometrics)
#if !DT_NODE_EXISTS(BIO_NODE)
#error "Copy your working AI10 overlay, with alias biometrics, into this app."
#endif
#define EVENT_CAPACITY 64U
#define INVENTORY_CAPACITY 2000U
#define FINISH_WATCHDOG_MS 20000
#define STOP_WATCHDOG_MS 15000
#define OP_TIMEOUT K_SECONDS(4)
#define ACTIVE_TIMEOUT K_SECONDS(20)

static const struct device *const bio = DEVICE_DT_GET(BIO_NODE);
K_MSGQ_DEFINE(events, sizeof(struct t2_event), EVENT_CAPACITY, 8);
K_SEM_DEFINE(terminal_seen, 0, 1);
K_SEM_DEFINE(stop_request, 0, 1);
K_SEM_DEFINE(stop_done, 0, 1);
K_MUTEX_DEFINE(runner_lock);
static atomic_t dropped_events;
static atomic_t callback_count;
static atomic_t context_errors;
static bool halted;
static uint16_t initial_ids[INVENTORY_CAPACITY];
static uint16_t current_ids[INVENTORY_CAPACITY];
static size_t initial_count;
static struct t2_event recorded[EVENT_CAPACITY];
/* Access stop_result/timestamps only after stop_done, never after a timeout. */
static int stop_result;
static int64_t stop_started_ms, stop_returned_ms;

struct trial {
	const struct shell *sh;
	unsigned int run;
	unsigned int op;
	const char *name;
	unsigned int failures;
};

static void row(struct trial *t, const char *kind, const char *name, const char *outcome,
		int rc, int64_t v1, int64_t v2, int64_t ms)
{
	shell_print(t->sh, "T2,%u,%s,%u,%s,%s,%s,%d,%lld,%lld,%lld", t->run, t->name,
		    t->op, kind, name, outcome, rc, (long long)v1, (long long)v2,
		    (long long)ms);
}

static bool check(struct trial *t, const char *name, bool ok, int rc, int64_t v1, int64_t v2)
{
	row(t, "check", name, ok ? "PASS" : "FAIL", rc, v1, v2, k_uptime_get());
	if (!ok) { t->failures++; }
	return ok;
}

static void callback(const struct device *dev, const struct biometric_event *e, void *data)
{
	struct t2_event copy = { .kind = T2_UNKNOWN, .status = e->status,
				.ms = k_uptime_get() };

	if (dev != bio || data != NULL || k_is_in_isr()) { atomic_inc(&context_errors); }
	switch (e->type) {
	case BIOMETRIC_EVENT_MATCH:
		copy.kind = T2_MATCH;
		copy.id = e->match.template_id;
		copy.modality = e->match.modality;
		break;
	case BIOMETRIC_EVENT_NO_MATCH: copy.kind = T2_NO_MATCH; break;
	case BIOMETRIC_EVENT_ENROLL_COMPLETE:
		copy.kind = T2_ENROLLED;
		copy.id = e->enrollment.template_id;
		copy.modality = e->enrollment.modality;
		break;
	case BIOMETRIC_EVENT_ERROR: copy.kind = T2_ERROR; break;
	case BIOMETRIC_EVENT_STOPPED: copy.kind = T2_STOPPED; break;
	default: break;
	}
	atomic_inc(&callback_count);
	if (k_msgq_put(&events, &copy, K_NO_WAIT) != 0) { atomic_inc(&dropped_events); }
	if (e->type == BIOMETRIC_EVENT_STOPPED) { k_sem_give(&terminal_seen); }
}

static void stop_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (true) {
		k_sem_take(&stop_request, K_FOREVER);
		stop_started_ms = k_uptime_get();
		stop_result = biometric_async_stop(bio);
		stop_returned_ms = k_uptime_get();
		k_sem_give(&stop_done);
	}
}
K_THREAD_DEFINE(stopper, 2048, stop_thread, NULL, NULL, NULL, 6, 0, 0);

static bool stop_checked(struct trial *t, int expected, const char *name)
{
	/* The driver may wait indefinitely for callbacks; keep shell supervision bounded. */
	k_sem_reset(&stop_done);
	k_sem_give(&stop_request);
	int ret = k_sem_take(&stop_done, K_MSEC(STOP_WATCHDOG_MS));

	if (!check(t, "stop_watchdog", ret == 0, ret, STOP_WATCHDOG_MS, 0)) {
		halted = true;
		return false;
	}
	row(t, "metric", "stop_call_ms", "INFO", stop_result,
	    stop_returned_ms - stop_started_ms, expected, stop_returned_ms);
	if (!check(t, name, stop_result == expected, stop_result, expected, 0)) {
		halted = true;
		return false;
	}
	return true;
}

static void begin_trace(void)
{
	/* Only call while previous callbacks are known to have finished. */
	k_msgq_purge(&events);
	k_sem_reset(&terminal_seen);
	atomic_clear(&dropped_events);
	atomic_clear(&callback_count);
	atomic_clear(&context_errors);
}

static const char *event_name(enum t2_kind kind)
{
	switch (kind) {
	case T2_MATCH: return "MATCH";
	case T2_NO_MATCH: return "NO_MATCH";
	case T2_ENROLLED: return "ENROLL_COMPLETE";
	case T2_ERROR: return "ERROR";
	case T2_STOPPED: return "STOPPED";
	default: return "UNKNOWN";
	}
}

static size_t dump_trace(struct trial *t)
{
	size_t count = 0U;

	while (count < ARRAY_SIZE(recorded) &&
	       k_msgq_get(&events, &recorded[count], K_NO_WAIT) == 0) {
		const struct t2_event *e = &recorded[count++];
		row(t, "event", event_name(e->kind), "INFO", e->status, e->id, e->modality, e->ms);
	}
	return count;
}

static bool finish_trace(struct trial *t, enum t2_expect expected)
{
	int ret = k_sem_take(&terminal_seen, K_MSEC(FINISH_WATCHDOG_MS));

	if (!check(t, "terminal_watchdog", ret == 0, ret, FINISH_WATCHDOG_MS, 0)) {
		/* Callback storage is static and remains valid after a halted run. */
		dump_trace(t);
		halted = true;
		return false;
	}
	/* STOPPED entry is not callback completion. Wait for idle via registration. */
	int64_t deadline = k_uptime_get() + 2000;
	do {
		ret = biometric_callback_set(bio, callback, NULL);
		if (ret != -EBUSY) { break; }
		k_sleep(K_MSEC(1));
	} while (k_uptime_get() < deadline);
	if (!check(t, "callback_quiescence", ret == 0, ret, 0, 0)) {
		dump_trace(t);
		halted = true;
		return false;
	}
	size_t count = dump_trace(t);
	unsigned int drops = (unsigned int)atomic_get(&dropped_events);
	struct t2_verdict v = t2_evaluate(recorded, count, drops, expected,
					 -ETIMEDOUT, -ECANCELED);

	bool recording_ok = drops == 0U && count == (size_t)atomic_get(&callback_count);
	check(t, "event_recording", recording_ok, 0, count, drops);
	check(t, "callback_context", atomic_get(&context_errors) == 0, 0,
	      atomic_get(&context_errors), 0);
	check(t, "lifecycle_trace", v.lifecycle, v.terminal_status, v.stops, count);
	check(t, "expected_outcome", v.outcome, v.terminal_status, expected, 0);
	return v.lifecycle && v.outcome && recording_ok && atomic_get(&context_errors) == 0;
}

static bool snapshot(struct trial *t, uint16_t *ids, size_t *count, const char *name)
{
	*count = 0U;
	int ret = biometric_template_list(bio, ids, INVENTORY_CAPACITY, count);
	bool valid = ret == 0 && *count <= INVENTORY_CAPACITY;

	if (valid) {
		for (size_t i = 0U; i < *count; i++) {
			if (ids[i] == 0U || ids[i] > INVENTORY_CAPACITY) { valid = false; }
			for (size_t j = 0U; j < i; j++) {
				if (ids[i] == ids[j]) { valid = false; }
			}
		}
	}
	if (!check(t, name, valid, ret, *count, 0)) {
		halted = true;
		return false;
	}
	return true;
}

static bool inventory_unchanged(struct trial *t)
{
	size_t count;
	if (!snapshot(t, current_ids, &count, "list_after_case")) { return false; }
	bool same = count == initial_count;

	for (size_t i = 0U; i < count; i++) {
		bool found = false;
		for (size_t j = 0U; j < initial_count; j++) {
			if (current_ids[i] == initial_ids[j]) { found = true; break; }
		}
		if (!found) { same = false; }
	}
	if (!check(t, "inventory_unchanged", same, 0, initial_count, count)) {
		for (size_t i = 0U; i < count; i++) {
			row(t, "info", "inventory_after", "INFO", 0, current_ids[i], i, k_uptime_get());
		}
		halted = true;
	}
	return same;
}

static bool start_operation(struct trial *t, bool enroll, bool continuous, k_timeout_t timeout)
{
	t->op++;
	begin_trace();
	int64_t before = k_uptime_get();
	int ret = enroll ? biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, timeout) :
		biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, continuous, timeout);
	int64_t after = k_uptime_get();

	row(t, "metric", "start_call_ms", "INFO", ret, after - before, continuous, after);
	if (!check(t, "accepted", ret == 0, ret, enroll, continuous)) {
		halted = true;
		return false;
	}
	return true;
}

static bool recovery_probe(struct trial *t)
{
	const char *saved = t->name;
	t->name = "restart_probe";
	/* No database/protocol API between the tested operation and this start. */
	bool ok = start_operation(t, false, true, ACTIVE_TIMEOUT);
	if (ok) {
		k_sleep(K_MSEC(100));
		ok = stop_checked(t, 0, "restart_stop");
		if (!halted) { ok = finish_trace(t, T2_CANCEL_IDENTIFY) && ok; }
		else { dump_trace(t); }
	}
	t->name = saved;
	return ok;
}

static bool reject(struct trial *t, const char *name, int actual, int expected)
{
	if (!check(t, name, actual == expected, actual, expected, 0)) {
		/* An unexpected acceptance can leave an operation running. Never reuse state. */
		halted = true;
		return false;
	}
	return true;
}

static bool rejections(struct trial *t)
{
	t->name = "rejections";
	begin_trace();
	if (!stop_checked(t, -EALREADY, "stop_idle")) { return false; }
	int ret = biometric_callback_set(bio, NULL, NULL);
	if (!reject(t, "unregister_idle", ret, 0)) { return false; }
	ret = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, OP_TIMEOUT);
	if (!reject(t, "enroll_missing_callback", ret, -EINVAL)) { return false; }
	ret = biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, false, OP_TIMEOUT);
	if (!reject(t, "identify_missing_callback", ret, -EINVAL)) { return false; }
	ret = biometric_callback_set(bio, callback, NULL);
	if (!reject(t, "register_idle", ret, 0)) { return false; }
	ret = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, K_NO_WAIT);
	if (!reject(t, "enroll_zero_timeout", ret, -EINVAL)) { return false; }
	ret = biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, false, K_FOREVER);
	if (!reject(t, "identify_infinite_timeout", ret, -EINVAL)) { return false; }
	ret = biometric_match_async(bio, BIOMETRIC_MATCH_VERIFY, 1, false, OP_TIMEOUT);
	if (!reject(t, "async_verify_unsupported", ret, -ENOTSUP)) { return false; }
	ret = biometric_enroll_async(bio, 1, OP_TIMEOUT);
	if (!reject(t, "explicit_id_unsupported", ret, -ENOTSUP)) { return false; }
	/* Bounded observation only: not a proof that no arbitrarily late event is possible. */
	k_sleep(K_MSEC(100));
	size_t count = dump_trace(t);
	bool ok = count == 0U && atomic_get(&callback_count) == 0 &&
		  atomic_get(&dropped_events) == 0;
	check(t, "rejected_requests_no_events", ok, 0, count, 100);
	if (!ok) { halted = true; }
	return ok;
}

struct case_spec {
	const char *name;
	bool enroll;
	bool cancel;
	int delay_ms;
	bool busy;
	enum t2_expect expected;
};
static const struct case_spec cases[] = {
	{ "enroll_timeout", true, false, 0, false, T2_ENROLL_TIMEOUT },
	{ "identify_once_no_sample", false, false, 0, false, T2_ONCE_NO_SAMPLE },
	{ "cancel_enroll_0ms", true, true, 0, false, T2_CANCEL_ENROLL },
	{ "cancel_enroll_1000ms", true, true, 1000, false, T2_CANCEL_ENROLL },
	{ "cancel_identify_0ms", false, true, 0, false, T2_CANCEL_IDENTIFY },
	{ "cancel_identify_1000ms", false, true, 1000, false, T2_CANCEL_IDENTIFY },
	{ "busy_identify", false, true, 1000, true, T2_CANCEL_IDENTIFY },
};

static bool run_case(struct trial *t, const struct case_spec *spec)
{
	unsigned int failures_before = t->failures;
	t->name = spec->name;
	if (!start_operation(t, spec->enroll, !spec->enroll && spec->cancel,
			     spec->cancel ? ACTIVE_TIMEOUT : OP_TIMEOUT)) { return false; }
	if (spec->cancel) {
		if (spec->delay_ms != 0) { k_sleep(K_MSEC(spec->delay_ms)); }
		if (spec->busy) {
			int ret = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, OP_TIMEOUT);
			if (!reject(t, "busy_enroll", ret, -EBUSY)) { return false; }
			ret = biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, true, OP_TIMEOUT);
			if (!reject(t, "busy_identify", ret, -EBUSY)) { return false; }
			ret = biometric_callback_set(bio, callback, NULL);
			if (!reject(t, "busy_callback_replace", ret, -EBUSY)) { return false; }
			size_t count = 0U;
			ret = biometric_template_list(bio, current_ids, INVENTORY_CAPACITY, &count);
			if (!reject(t, "busy_database_list", ret, -EBUSY)) { return false; }
		}
		row(t, "info", "stop_requested", "INFO", 0, spec->delay_ms, 0, k_uptime_get());
		if (!stop_checked(t, 0, "stop_result")) {
			dump_trace(t);
			return false;
		}
	}
	bool ok = finish_trace(t, spec->expected);
	/* If quiescent, inspect storage even after an unexpected result; never delete. */
	if (!halted && ok) { ok = recovery_probe(t); }
	if (!halted) { ok = inventory_unchanged(t) && ok; }
	if (!halted && ok) { ok = stop_checked(t, -EALREADY, "stop_again_idle"); }
	if (!ok || t->failures != failures_before) { halted = true; }
	row(t, "summary", "case", ok && t->failures == failures_before ? "PASS" : "FAIL",
	    ok ? 0 : -EIO, t->failures - failures_before, halted, k_uptime_get());
	return ok && !halted;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct biometric_capabilities caps = {0};
	int ret = device_is_ready(bio) ? biometric_get_capabilities(bio, &caps) : -ENODEV;
	if (ret != 0) { shell_error(sh, "Device/capabilities error: %d", ret); return ret; }
	shell_print(sh, "T2A v1.0; device=%s; modalities=%u; async=%u; capacity=%u",
		    bio->name, caps.supported_modalities, caps.async_operations, caps.max_templates);
	shell_print(sh, "No sample required. Keep face and palm out of view. Use: t2 run <number>");
	return 0;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char *end;
	unsigned long number = strtoul(argv[1], &end, 10);
	if (argv[1][0] == '\0' || *end != '\0' || number == 0UL || number > 1000000UL) {
		return -EINVAL;
	}
	if (k_mutex_lock(&runner_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	struct trial t = { .sh = sh, .run = (unsigned int)number, .name = "setup" };
	struct biometric_capabilities caps = {0};
	int ret;

	if (!check(&t, "runner_available", !halted, halted ? -EBUSY : 0, 0, 0)) { goto done; }
	ret = device_is_ready(bio) ? biometric_get_capabilities(bio, &caps) : -ENODEV;
	if (!check(&t, "capabilities", ret == 0, ret, caps.supported_modalities,
		   caps.async_operations)) { goto done; }
	if (!check(&t, "ai10_profile", caps.max_templates == INVENTORY_CAPACITY &&
		   caps.supported_modalities == (BIOMETRIC_MODALITY_FACE | BIOMETRIC_MODALITY_PALM) &&
		   (caps.async_operations & (BIOMETRIC_ASYNC_ENROLL | BIOMETRIC_ASYNC_IDENTIFY)) ==
		   (BIOMETRIC_ASYNC_ENROLL | BIOMETRIC_ASYNC_IDENTIFY), 0, caps.max_templates, 0)) {
		goto done;
	}
	shell_print(sh, "Run %u: keep face and palm OUT OF VIEW. Starting in 3 seconds.", t.run);
	k_sleep(K_SECONDS(3));
	if (!snapshot(&t, initial_ids, &initial_count, "list_before_suite")) { goto done; }
	for (size_t i = 0U; i < initial_count; i++) {
		row(&t, "info", "inventory_before", "INFO", 0, initial_ids[i], i, k_uptime_get());
	}
	if (!rejections(&t)) { goto done; }
	for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
		if (!run_case(&t, &cases[i])) { break; }
	}
 done:
	if (t.failures != 0U) { halted = true; }
	t.name = "suite";
	row(&t, "summary", "suite", t.failures == 0U && !halted ? "PASS" : "FAIL",
	    t.failures == 0U && !halted ? 0 : -EIO, t.failures, halted, k_uptime_get());
	if (halted) {
		shell_error(sh, "Halted. Save log. Inspect connection and template state before reboot/retry.");
	}
	k_mutex_unlock(&runner_lock);
	return halted ? -EIO : 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t2_commands,
	SHELL_CMD_ARG(info, NULL, "Show Test 2A capabilities.", cmd_info, 1, 0),
	SHELL_CMD_ARG(run, NULL, "Run no-sample lifecycle suite: t2 run <number>.", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(t2, &t2_commands, "BASE Test 2A", NULL);

int main(void)
{
	printk("BASE Test 2A v1.0 ready. Use 't2 info'. Keep face/palm out of view.\n");
	return 0;
}
