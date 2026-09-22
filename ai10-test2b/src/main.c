/*
 * SPDX-License-Identifier: Apache-2.0
 * BASE Test 2B: AI10 terminal-callback barrier checks, harness version 1.0.
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
K_SEM_DEFINE(stop_entered, 0, 1);
K_SEM_DEFINE(gate_entered, 0, 1);
K_SEM_DEFINE(gate_release, 0, 1);
#define GATE_WATCHDOG_MS 5000
static atomic_t gate_arm, gate_active, gate_expired;
static atomic_t order_counter, callback_before_return_order, primary_return_order;
static atomic_t primary_returned;
static int64_t gate_entered_ms, callback_before_return_ms;
static int64_t release_ms;
static bool hold_next_operation;
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
	shell_print(t->sh, "T2B,%u,%s,%u,%s,%s,%s,%d,%lld,%lld,%lld", t->run, t->name,
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
	if (e->type == BIOMETRIC_EVENT_STOPPED) {
		/* Deliberately slow callback: a bounded experimental perturbation only. */
		if (atomic_cas(&gate_arm, 1, 0)) {
			gate_entered_ms = k_uptime_get();
			atomic_set(&gate_active, 1);
			k_sem_give(&gate_entered);
			int ret = k_sem_take(&gate_release, K_MSEC(GATE_WATCHDOG_MS));
			if (ret != 0) { atomic_set(&gate_expired, 1); }
			callback_before_return_ms = k_uptime_get();
			atomic_set(&callback_before_return_order, atomic_inc(&order_counter) + 1);
			atomic_clear(&gate_active);
		}
		k_sem_give(&terminal_seen);
	}
}

static void stop_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (true) {
		k_sem_take(&stop_request, K_FOREVER);
		stop_started_ms = k_uptime_get();
		k_sem_give(&stop_entered);
		stop_result = biometric_async_stop(bio);
		stop_returned_ms = k_uptime_get();
		atomic_set(&primary_return_order, atomic_inc(&order_counter) + 1);
		atomic_set(&primary_returned, 1);
		k_sem_give(&stop_done);
	}
}
K_THREAD_DEFINE(stopper, 2048, stop_thread, NULL, NULL, NULL, 6, 0, 0);

static bool stop_start(struct trial *t)
{
	k_sem_reset(&stop_done);
	k_sem_reset(&stop_entered);
	atomic_clear(&primary_returned);
	atomic_clear(&primary_return_order);
	k_sem_give(&stop_request);
	int ret = k_sem_take(&stop_entered, K_SECONDS(2));
	if (!check(t, "stop_helper_started", ret == 0, ret, 0, 0)) {
		halted = true;
		return false;
	}
	return true;
}

static bool stop_finish(struct trial *t, int expected, const char *name)
{
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

static bool stop_checked(struct trial *t, int expected, const char *name)
{
	return stop_start(t) && stop_finish(t, expected, name);
}

static void begin_trace(void)
{
	/* Only call while previous callbacks are known to have finished. */
	k_msgq_purge(&events);
	k_sem_reset(&terminal_seen);
	atomic_clear(&dropped_events);
	atomic_clear(&callback_count);
	atomic_clear(&context_errors);
	k_sem_reset(&gate_entered);
	k_sem_reset(&gate_release);
	atomic_clear(&gate_arm);
	atomic_clear(&gate_active);
	atomic_clear(&gate_expired);
	atomic_clear(&order_counter);
	atomic_clear(&callback_before_return_order);
	atomic_clear(&primary_return_order);
	atomic_clear(&primary_returned);
	gate_entered_ms = 0;
	callback_before_return_ms = 0;
	release_ms = 0;
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
	atomic_set(&gate_arm, hold_next_operation ? 1 : 0);
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
	hold_next_operation = false;
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

/* Independent probe thread: even an incorrectly blocking API cannot trap the shell. */
enum probe_command { PROBE_ENROLL, PROBE_IDENTIFY, PROBE_REGISTER, PROBE_LIST, PROBE_STOP };
K_SEM_DEFINE(probe_request, 0, 1);
K_SEM_DEFINE(probe_done, 0, 1);
static enum probe_command probe_command;
static int probe_result;
static bool probe_active_before, probe_active_after;
static uint16_t probe_ids[INVENTORY_CAPACITY];

static void probe_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (true) {
		k_sem_take(&probe_request, K_FOREVER);
		probe_active_before = atomic_get(&gate_active) != 0;
		size_t count = 0U;
		switch (probe_command) {
		case PROBE_ENROLL:
			probe_result = biometric_enroll_async(bio, BIOMETRIC_ID_AUTO, ACTIVE_TIMEOUT);
			break;
		case PROBE_IDENTIFY:
			probe_result = biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY,
							    0, true, ACTIVE_TIMEOUT);
			break;
		case PROBE_REGISTER:
			probe_result = biometric_callback_set(bio, callback, NULL);
			break;
		case PROBE_LIST:
			probe_result = biometric_template_list(bio, probe_ids, ARRAY_SIZE(probe_ids), &count);
			break;
		case PROBE_STOP:
			probe_result = biometric_async_stop(bio);
			break;
		default: probe_result = -EINVAL; break;
		}
		probe_active_after = atomic_get(&gate_active) != 0;
		k_sem_give(&probe_done);
	}
}
K_THREAD_DEFINE(prober, 2048, probe_thread, NULL, NULL, NULL, 7, 0, 0);

static bool probe_busy(struct trial *t, enum probe_command command, const char *name)
{
	probe_command = command;
	k_sem_reset(&probe_done);
	k_sem_give(&probe_request);
	int ret = k_sem_take(&probe_done, K_SECONDS(2));
	if (!check(t, "probe_watchdog", ret == 0, ret, command, 2000)) {
		halted = true;
		return false;
	}
	bool active = probe_active_before && probe_active_after &&
		      atomic_get(&gate_active) != 0 && atomic_get(&gate_expired) == 0;
	bool ok = check(t, "probe_during_callback", active, 0, command, 0);
	ok = check(t, name, probe_result == -EBUSY, probe_result, -EBUSY, command) && ok;
	if (!ok) { halted = true; }
	return ok;
}

static void release_gate(void)
{
	release_ms = k_uptime_get();
	k_sem_give(&gate_release);
}

static bool run_barrier_case(struct trial *t, bool natural)
{
	unsigned int failures_before = t->failures;
	t->name = natural ? "natural_terminal" : "cancel_terminal";
	hold_next_operation = true;
	bool ok = false;
	bool primary_started = false;
	int observation_ms = natural ? 100 : 500;

	/* Natural: timeout enrollment without calling stop until STOPPED is held.
	 * Cancellation: start stop while continuous identification is still active. */
	if (!start_operation(t, natural, !natural, natural ? OP_TIMEOUT : ACTIVE_TIMEOUT)) {
		goto out;
	}
	if (!natural) {
		k_sleep(K_MSEC(100));
		primary_started = true;
		if (!stop_start(t)) { goto out; }
	}
	int ret = k_sem_take(&gate_entered, K_MSEC(FINISH_WATCHDOG_MS));
	if (!check(t, "terminal_callback_entered", ret == 0, ret, 0, 0)) {
		halted = true;
		goto out;
	}
	if (!check(t, "callback_gate_active", atomic_get(&gate_active) != 0 &&
		   atomic_get(&gate_expired) == 0, 0, 0, 0)) {
		halted = true;
		goto out;
	}
	/* These probes deliberately precede stop in the natural case. Thus EBUSY
	 * cannot be explained merely by the stop caller holding the driver mutex. */
	if (!probe_busy(t, PROBE_ENROLL, "busy_enroll") ||
	    !probe_busy(t, PROBE_IDENTIFY, "busy_identify") ||
	    !probe_busy(t, PROBE_REGISTER, "busy_callback_replace") ||
	    !probe_busy(t, PROBE_LIST, "busy_database_list")) { goto out; }
	if (natural) {
		primary_started = true;
		if (!stop_start(t)) { goto out; }
	}
	/* Separate caller tests stop contention; the primary stop must be pending. */
	if (!probe_busy(t, PROBE_STOP, "second_stop_busy")) { goto out; }
	if (!check(t, "stop_pending_before_window", atomic_get(&primary_returned) == 0,
		   0, 0, 0)) { halted = true; goto out; }
	int64_t window_start = k_uptime_get();
	k_sleep(K_MSEC(observation_ms));
	int64_t window_end = k_uptime_get();
	bool held = atomic_get(&gate_active) != 0 && atomic_get(&gate_expired) == 0;
	bool pending = atomic_get(&primary_returned) == 0;
	row(t, "metric", "pending_observation_ms", "INFO", 0,
	    window_end - window_start, observation_ms, window_end);
	if (!check(t, "stop_waits_for_callback", held && pending, 0, held, pending)) {
		halted = true;
		goto out;
	}
	release_gate();
	if (!stop_finish(t, 0, "stop_after_release")) { goto out; }
	if (!finish_trace(t, natural ? T2_ENROLL_TIMEOUT : T2_CANCEL_IDENTIFY)) {
		halted = true;
		goto out;
	}
	long before_return = atomic_get(&callback_before_return_order);
	long after_stop = atomic_get(&primary_return_order);
	if (!check(t, "callback_marker_before_stop_return", before_return > 0 &&
		   after_stop > before_return, 0, before_return, after_stop)) {
		halted = true; goto out;
	}
	if (!check(t, "gate_released_without_watchdog", atomic_get(&gate_expired) == 0 &&
		   atomic_get(&gate_active) == 0, 0, 0, 0)) { halted = true; goto out; }
	row(t, "metric", "callback_gate_span_ms", "INFO", 0,
	    callback_before_return_ms - gate_entered_ms, observation_ms, callback_before_return_ms);
	row(t, "metric", "release_to_stop_return_ms", "INFO", 0,
	    stop_returned_ms - release_ms, 0, stop_returned_ms);
	ok = recovery_probe(t);
	if (!halted) { ok = inventory_unchanged(t) && ok; }
	if (!halted && ok) { ok = stop_checked(t, -EALREADY, "stop_again_idle"); }
 out:
	/* Unblock a held callback even on a failed probe. Never abort driver/helper
	 * threads or reuse shared state after uncertain completion. */
	if (!ok) {
		atomic_clear(&gate_arm);
		k_sem_give(&gate_release);
		halted = true;
		dump_trace(t);
	}
	row(t, "summary", "case", ok && !halted ? "PASS" : "FAIL", ok ? 0 : -EIO,
	    t->failures - failures_before, primary_started, k_uptime_get());
	return ok && !halted;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct biometric_capabilities caps = {0};
	int ret = device_is_ready(bio) ? biometric_get_capabilities(bio, &caps) : -ENODEV;
	if (ret != 0) { shell_error(sh, "Device/capabilities error: %d", ret); return ret; }
	shell_print(sh, "T2B v1.0; device=%s; modalities=%u; async=%u; capacity=%u",
		    bio->name, caps.supported_modalities, caps.async_operations, caps.max_templates);
	shell_print(sh, "No sample. Use: t2b run <number>. STOPPED callbacks are deliberately held.");
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
	ret = biometric_callback_set(bio, callback, NULL);
	if (!check(&t, "register_idle", ret == 0, ret, 0, 0)) { goto done; }
	if (run_barrier_case(&t, true)) { run_barrier_case(&t, false); }
 done:
	if (t.failures != 0U) { halted = true; }
	if (halted) { atomic_clear(&gate_arm); k_sem_give(&gate_release); }
	t.name = "suite";
	row(&t, "summary", "suite", !halted ? "PASS" : "FAIL", !halted ? 0 : -EIO,
	    t.failures, halted, k_uptime_get());
	if (halted) {
		shell_error(sh, "Halted. Save log. Inspect connection/state before reboot and retry.");
	}
	k_mutex_unlock(&runner_lock);
	return halted ? -EIO : 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t2b_commands,
	SHELL_CMD_ARG(info, NULL, "Show Test 2B capabilities.", cmd_info, 1, 0),
	SHELL_CMD_ARG(run, NULL, "Run callback barrier suite: t2b run <number>.", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(t2b, &t2b_commands, "BASE Test 2B", NULL);

int main(void)
{
	printk("BASE Test 2B v1.0 ready. Use 't2b info'. Keep face/palm out of view.\n");
	return 0;
}
