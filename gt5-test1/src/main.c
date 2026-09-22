/* SPDX-License-Identifier: Apache-2.0
 * GT5 functional evaluation v1.0; uses only the public biometric API.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IDS 200U
#define BIO_NODE DT_ALIAS(biometrics)
#if !DT_NODE_EXISTS(BIO_NODE)
#error "The overlay must define aliases { biometrics = &fingerprint0; };"
#endif
static const struct device *const bio = DEVICE_DT_GET(BIO_NODE);
/* Full configured inventory: the build helper first fixes template_list to
 * propagate communication errors. Never allocate from an incomplete scan.
 */
static uint16_t before[MAX_IDS], after[MAX_IDS];
static size_t before_count;
static uint32_t trial, last_trial;
static uint16_t owned_id;
static bool halted;

static void row(const struct shell *sh, const char *check, const char *verdict,
		int rc, unsigned int a, unsigned int b)
{
	shell_print(sh, "G1,%u,%s,%s,%d,%u,%u,%u,%lld", (unsigned int)trial,
		    check, verdict, rc, (unsigned int)owned_id, a, b,
		    (long long)k_uptime_get());
}

static bool expect(const struct shell *sh, const char *check, int actual, int expected)
{
	bool ok = actual == expected;
	row(sh, check, ok ? "PASS" : "FAIL", actual, 0, 0);
	return ok;
}

static bool property(const struct shell *sh, const char *check, bool ok,
		     unsigned int a, unsigned int b)
{
	row(sh, check, ok ? "PASS" : "FAIL", ok ? 0 : -EPROTO, a, b);
	return ok;
}

static bool contains(const uint16_t *ids, size_t n, uint16_t id)
{
	for (size_t i = 0; i < n; ++i) {
		if (ids[i] == id) { return true; }
	}
	return false;
}

static bool inventory(const struct shell *sh, const char *name, uint16_t *ids,
		      size_t *count, uint16_t capacity)
{
	*count = 0;
	int ret = biometric_template_list(bio, ids, MAX_IDS, count);
	if (!expect(sh, name, ret, 0)) { return false; }
	bool valid = *count <= MAX_IDS;
	if (valid) {
		for (size_t i = 0; i < *count; ++i) {
			if (ids[i] == 0 || ids[i] > MAX_IDS || ids[i] > capacity ||
			    contains(ids, i, ids[i])) { valid = false; break; }
		}
	}
	if (!property(sh, "inventory_valid", valid, (unsigned int)*count, MAX_IDS)) {
		return false;
	}
	row(sh, "inventory_count", "INFO", 0, (unsigned int)*count, MAX_IDS);
	for (size_t i = 0; i < *count; ++i) {
		row(sh, "inventory_id", "INFO", 0, ids[i], (unsigned int)i);
	}
	return true;
}

static bool same_inventory(const uint16_t *ids, size_t count, bool added)
{
	if (count != before_count + (added ? 1U : 0U)) { return false; }
	for (size_t i = 0; i < before_count; ++i) {
		if (!contains(ids, count, before[i])) { return false; }
	}
	return contains(ids, count, owned_id) == added;
}

static bool capabilities(const struct shell *sh, struct biometric_capabilities *caps)
{
	if (!expect(sh, "device_ready", device_is_ready(bio) ? 0 : -ENODEV, 0)) {
		return false;
	}
	int ret = biometric_get_capabilities(bio, caps);
	if (!expect(sh, "capabilities", ret, 0)) { return false; }
	row(sh, "caps_modalities_async", "INFO", 0, caps->supported_modalities,
	    caps->async_operations);
	row(sh, "caps_samples_capacity", "INFO", 0, caps->enrollment_samples_required,
	    caps->max_templates);
	row(sh, "caps_storage_template_bytes", "INFO", 0, caps->storage_modes,
	    caps->template_size);
	return property(sh, "gt5_capability_contract",
		caps->supported_modalities == BIOMETRIC_MODALITY_FINGERPRINT &&
		caps->async_operations == 0 && caps->enrollment_samples_required == 3 &&
		(caps->storage_modes & BIOMETRIC_STORAGE_DEVICE) != 0 && caps->max_templates == MAX_IDS && caps->template_size == 498,
		caps->supported_modalities, caps->enrollment_samples_required);
}

static void callback(const struct device *dev, const struct biometric_event *event, void *user)
{
	ARG_UNUSED(dev); ARG_UNUSED(event); ARG_UNUSED(user);
	/* With the supplied GT5 driver, callback registration returns -ENOSYS. */
}

static void lift(const struct shell *sh)
{
	shell_print(sh, "LIFT your finger now. Keep it OFF for 3 seconds.");
	k_sleep(K_SECONDS(3));
}

static bool match(const struct shell *sh, enum biometric_match_mode mode)
{
	struct biometric_match_result result = {0};
	const char *name = mode == BIOMETRIC_MATCH_VERIFY ? "verify" : "identify";
	lift(sh);
	shell_print(sh, "PLACE the SAME finger for %s (within 30 seconds).", name);
	int ret = biometric_match(bio, mode, owned_id, K_SECONDS(30), &result);
	if (!expect(sh, name, ret, 0)) { return false; }
	row(sh, "match_score_quality", "INFO", result.confidence, result.image_quality,
	    result.modality);
	return property(sh, "match_identity_modality",
		result.template_id == owned_id && result.modality == BIOMETRIC_MODALITY_FINGERPRINT,
		result.template_id, result.modality);
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct biometric_capabilities caps = {0};
	size_t count;
	trial = 0;
	shell_print(sh, "Device: %s; configured inventory: IDs 1..200", bio->name);
	if (!capabilities(sh, &caps)) { return -EIO; }
	return inventory(sh, "list_info", after, &count, caps.max_templates) ? 0 : -EIO;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char *end;
	errno = 0;
	unsigned long value = strtoul(argv[1], &end, 10);
	if (errno || argv[1][0] < '0' || argv[1][0] > '9' || *end || value == 0 ||
	    value > UINT32_MAX || value <= last_trial) {
		shell_error(sh, "Use a new increasing positive trial number: g1 run 1");
		return -EINVAL;
	}
	if (halted) {
		shell_error(sh, "Previous trial failed. Keep the log and inspect state before reset/retry.");
		return -EBUSY;
	}
	trial = (uint32_t)value;
	last_trial = trial;
	owned_id = 0;
	struct biometric_capabilities caps = {0};
	struct biometric_capture_result capture = {0};
	size_t count;
	bool started = false;

	row(sh, "begin", "INFO", 0, 1, 0);
	shell_print(sh, "Scanning all 200 template slots; please wait.");
	if (!capabilities(sh, &caps)) { goto fail; }
	if (!inventory(sh, "list_before", before, &before_count, caps.max_templates)) {
		goto fail;
	}
	unsigned int limit = caps.max_templates < MAX_IDS ? caps.max_templates : MAX_IDS;
	for (unsigned int id = 1; id <= limit; ++id) {
		if (!contains(before, before_count, (uint16_t)id)) { owned_id = (uint16_t)id; break; }
	}
	if (!property(sh, "free_id_in_inventory", owned_id != 0, owned_id, limit)) { goto fail; }
	row(sh, "inventory_scope", "INFO", 0, limit, caps.max_templates);

	if (!expect(sh, "unsupported_callback", biometric_callback_set(bio, callback, NULL), -ENOSYS)) {
		goto fail;
	}
	if (!expect(sh, "unsupported_async_enroll",
		    biometric_enroll_async(bio, owned_id, K_SECONDS(1)), -ENOSYS)) { goto fail; }
	if (!expect(sh, "unsupported_async_identify",
		    biometric_match_async(bio, BIOMETRIC_MATCH_IDENTIFY, 0, false, K_SECONDS(1)),
		    -ENOSYS)) { goto fail; }
	if (!expect(sh, "unsupported_async_verify",
		    biometric_match_async(bio, BIOMETRIC_MATCH_VERIFY, owned_id, false, K_SECONDS(1)),
		    -ENOSYS)) { goto fail; }
	if (!expect(sh, "unsupported_async_stop", biometric_async_stop(bio), -ENOSYS)) { goto fail; }

	if (!expect(sh, "enroll_start", biometric_enroll_start(bio, owned_id), 0)) { goto fail; }
	started = true;
	for (unsigned int sample = 1; sample <= 3; ++sample) {
		if (sample > 1) { lift(sh); }
		shell_print(sh, "PLACE finger: enrollment sample %u/3 (within 30 seconds).", sample);
		shell_print(sh, "Hold until LIFT. The driver may wait 5 seconds before that prompt.");
		memset(&capture, 0, sizeof(capture));
		if (!expect(sh, "enroll_capture",
			    biometric_enroll_capture(bio, K_SECONDS(30), &capture), 0)) { goto fail; }
		if (!property(sh, "capture_progress",
			      capture.samples_captured == sample && capture.samples_required == 3,
			      capture.samples_captured, capture.samples_required)) { goto fail; }
		/* This driver does not populate capture.template_id. Report it rather
		 * than silently substituting the ID requested by the application.
		 */
		row(sh, "capture_id_quality", "INFO", 0, capture.template_id, capture.quality);
		if (sample == 3) {
			row(sh, "capture3_commit_acknowledged", "INFO", 0, owned_id, 0);
			shell_print(sh, "LIFT finger now; checking the stored template.");
		}
	}
	/* GT5 commits during capture 3; finalize is not the commit point. */
	if (!expect(sh, "enroll_finalize", biometric_enroll_finalize(bio), 0)) { goto fail; }
	started = false;
	if (!inventory(sh, "list_after_enroll", after, &count, caps.max_templates)) { goto fail; }
	if (!property(sh, "only_test_id_added", same_inventory(after, count, true),
		      (unsigned int)before_count, (unsigned int)count)) { goto fail; }
	if (!match(sh, BIOMETRIC_MATCH_VERIFY) || !match(sh, BIOMETRIC_MATCH_IDENTIFY)) { goto fail; }
	if (!expect(sh, "delete_test_id", biometric_template_delete(bio, owned_id), 0)) { goto fail; }
	if (!inventory(sh, "list_after_delete", after, &count, caps.max_templates)) { goto fail; }
	if (!property(sh, "inventory_restored", same_inventory(after, count, false),
		      (unsigned int)before_count, (unsigned int)count)) { goto fail; }
	row(sh, "summary", "PASS", 0, 0, 0);
	shell_print(sh, "Trial complete. Lift finger. The test template was deleted.");
	return 0;
fail:
	halted = true;
	if (started) {
		/* Abort resets driver state only. Capture 3 may already have committed. */
		row(sh, "abort_after_failure", "INFO", biometric_enroll_abort(bio), 0, 0);
	}
	row(sh, "summary", "FAIL", -EIO, 0, 0);
	shell_error(sh, "Halted. Retain log. Test ID %u may need inspection; no automatic deletion on failure.",
		    (unsigned int)owned_id);
	return -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(commands,
	SHELL_CMD_ARG(info, NULL, "Capabilities and visible inventory.", cmd_info, 1, 0),
	SHELL_CMD_ARG(run, NULL, "Functional trial: g1 run <trial>.", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(g1, &commands, "GT5 functional evaluation", NULL);

int main(void)
{
	printk("GT5 Test 1 v1.0 ready. Use g1 info, then g1 run 1.\n");
	return 0;
}
