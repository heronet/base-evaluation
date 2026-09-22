/* SPDX-License-Identifier: Apache-2.0
 * BASE GT5 Test 3 v1.0: timer-release scheduling interference, one CPU.
 * Derived from corrected AI10 Test 3 v1.1; retains its failed-receive guard.
 * No biometric samples. No enrollment/deletion. No output during measurement.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/biometrics.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#define JOBS 300U
#define PERIOD_MS 10U
#define LOAD_PERIOD_MS 7U
#define BIO_PRIORITY 5
#define LOAD_PRIORITY 4
#define BIO_NODE DT_ALIAS(biometrics)
#if !DT_NODE_EXISTS(BIO_NODE)
#error "Use the GT5 overlay with alias biometrics."
#endif
BUILD_ASSERT(CONFIG_NUM_PREEMPT_PRIORITIES > 8);

enum mode { IDLE, SYNC };
static const char *const mode_names[] = { "idle", "sync" };
static const struct device *const bio = DEVICE_DT_GET(BIO_NODE);
struct release { uint32_t seq, cycles; };
struct sample { uint32_t release, start, finish; };
static struct sample samples[JOBS];
K_MSGQ_DEFINE(releases, sizeof(struct release), JOBS, 4);
K_SEM_DEFINE(jobs_done, 0, 1);
K_SEM_DEFINE(load_request, 0, 1);
K_SEM_DEFINE(load_tick, 0, 1024);
K_SEM_DEFINE(load_done, 0, 1);
K_SEM_DEFINE(bio_request, 0, 1);
K_SEM_DEFINE(bio_entered, 0, 1);
K_SEM_DEFINE(bio_done, 0, 1);
K_MUTEX_DEFINE(runner_lock);
static atomic_t timer_count, dropped, measuring, load_running, bio_running;
static atomic_t processed_jobs, receive_errors;
static atomic_t load_releases, load_executed, bio_error;
static unsigned int bio_started, bio_completed, bio_in_window;
static uint32_t victim_iters, load_iters, clock_hz, victim_cal_us, load_cal_us;
static bool halted;
static unsigned int last_run;

/* Fixed instruction work: preemption does not shorten its iteration count. */
static void work(uint32_t iterations)
{
	volatile uint32_t x = 0x12345678U;
	for (uint32_t i = 0; i < iterations; ++i) {
		x = x * 1664525U + 1013904223U;
	}
}

static uint32_t elapsed_us(uint32_t cycles)
{
	return (uint32_t)((uint64_t)cycles * 1000000U / clock_hz);
}

static uint32_t measure_work(uint32_t iterations)
{
	uint32_t start = k_cycle_get_32();
	work(iterations);
	return k_cycle_get_32() - start;
}

/* Median of five preserves an ordinary-interrupt calibration, without irq_lock. */
static uint32_t median_work(uint32_t iterations)
{
	uint32_t values[5];
	for (unsigned int i = 0; i < ARRAY_SIZE(values); ++i) {
		values[i] = measure_work(iterations);
		for (unsigned int j = i; j > 0 && values[j] < values[j - 1]; --j) {
			uint32_t tmp = values[j];
			values[j] = values[j - 1];
			values[j - 1] = tmp;
		}
	}
	return values[2];
}

static int calibrate(uint32_t target_us, uint32_t *iterations, uint32_t *actual_us)
{
	uint32_t n = 1024U, duration;
	do {
		duration = elapsed_us(median_work(n));
		if (duration >= 100U) { break; }
		n *= 2U;
	} while (n <= 1048576U);
	if (duration == 0U) { return -ERANGE; }
	uint64_t scaled = (uint64_t)n * target_us / duration;
	if (scaled == 0U || scaled > 10000000U) { return -ERANGE; }
	*iterations = (uint32_t)scaled;
	*actual_us = elapsed_us(median_work(*iterations));
	/* Report and reject a grossly unrepresentative calibration. */
	return (*actual_us >= target_us * 8U / 10U &&
		*actual_us <= target_us * 12U / 10U) ? 0 : -ERANGE;
}

static void release_expiry(struct k_timer *timer)
{
	struct release r = { .cycles = k_cycle_get_32() };
	r.seq = (uint32_t)atomic_inc(&timer_count);
	if (r.seq < JOBS && k_msgq_put(&releases, &r, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
	}
	if (r.seq + 1U >= JOBS) { k_timer_stop(timer); }
}
K_TIMER_DEFINE(release_timer, release_expiry, NULL);

static void load_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	atomic_inc(&load_releases);
	k_sem_give(&load_tick);
}
K_TIMER_DEFINE(load_timer, load_expiry, NULL);

static void victim_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		struct release r;
		int ret = k_msgq_get(&releases, &r, K_FOREVER);
		/* Purging a queue can wake a blocked receiver without delivering data.
		 * Never inspect r, execute work, or signal completion after a failed get.
		 */
		if (ret != 0) {
			if (ret != -ENOMSG) { atomic_inc(&receive_errors); }
			continue;
		}
		if (r.seq >= JOBS) {
			atomic_inc(&receive_errors);
			continue;
		}
		uint32_t start = k_cycle_get_32();
		work(victim_iters);
		uint32_t finish = k_cycle_get_32();
		samples[r.seq] = (struct sample){ r.cycles, start, finish };
		atomic_inc(&processed_jobs);
		if (r.seq + 1U == JOBS) { k_sem_give(&jobs_done); }
	}
}
K_THREAD_DEFINE(victim_tid, 2048, victim_thread, NULL, NULL, NULL, 2, 0, 0);

static void load_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&load_request, K_FOREVER);
		for (;;) {
			k_sem_take(&load_tick, K_FOREVER);
			if (!atomic_get(&load_running)) { break; }
			work(load_iters);
			atomic_inc(&load_executed);
		}
		k_sem_give(&load_done);
	}
}
K_THREAD_DEFINE(load_tid, 2048, load_thread, NULL, NULL, NULL, LOAD_PRIORITY, 0, 0);

static void bio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&bio_request, K_FOREVER);
		bool first = true;
		while (atomic_get(&bio_running)) {
			int ret;
			bio_started++;
			if (first) { k_sem_give(&bio_entered); first = false; }
			struct biometric_match_result result;
			ret = biometric_match(bio, BIOMETRIC_MATCH_IDENTIFY, 0,
					      K_SECONDS(1), &result);
			if (ret == -ETIMEDOUT) { ret = 0; }
			else if (ret == 0) { ret = -EPROTO; }
			if (ret != 0) { atomic_set(&bio_error, ret); }
			if (atomic_get(&bio_error)) { break; }
			bio_completed++;
			if (atomic_get(&measuring)) { bio_in_window++; }
		}
		k_sem_give(&bio_done);
	}
}
K_THREAD_DEFINE(bio_tid, 3072, bio_thread, NULL, NULL, NULL, BIO_PRIORITY, 0, 0);

static void meta(const struct shell *sh, unsigned int run, unsigned int cell,
		 const char *name, int64_t a, int64_t b)
{
	shell_print(sh, "G3,M,%u,%u,%s,%lld,%lld", run, cell, name,
		    (long long)a, (long long)b);
}

static void verdict(const struct shell *sh, unsigned int run, unsigned int cell,
		    const char *name, bool ok, int rc)
{
	shell_print(sh, "G3,V,%u,%u,%s,%s,%d", run, cell, name, ok ? "PASS" : "FAIL", rc);
}

static int do_cell(const struct shell *sh, unsigned int run, unsigned int cell,
		   enum mode mode, int priority, unsigned int load)
{
	meta(sh, run, cell, "condition", mode, priority);
	meta(sh, run, cell, "load_percent", load, LOAD_PRIORITY);
	shell_print(sh, "Cell %u: %s, task priority %d, offered load %u%%. Keep samples away; wait.",
		    cell, mode_names[mode], priority, load);
	/* Let preceding shell output drain before starting either workload. */
	k_sleep(K_MSEC(300));
	atomic_clear(&timer_count); atomic_clear(&dropped);
	atomic_clear(&processed_jobs); atomic_clear(&receive_errors);
	atomic_clear(&load_releases); atomic_clear(&load_executed);
	atomic_clear(&bio_error);
	k_sem_reset(&jobs_done); k_sem_reset(&load_tick); k_sem_reset(&load_done);
	k_sem_reset(&bio_done); k_sem_reset(&bio_entered);
	/* No purge: the prior successful window consumed every queued release.
	 * A purge would unnecessarily wake the receiver waiting for the next job.
	 */
	bio_started = 0; bio_completed = 0; bio_in_window = 0;
	k_thread_priority_set(victim_tid, priority);
	if (mode != IDLE) {
		atomic_set(&bio_running, 1);
		k_sem_give(&bio_request);
		if (k_sem_take(&bio_entered, K_SECONDS(2)) != 0) { return -ETIMEDOUT; }
	}
	k_sleep(K_MSEC(200));
	if (load) {
		atomic_set(&load_running, 1);
		k_sem_give(&load_request);
		k_timer_start(&load_timer, K_MSEC(LOAD_PERIOD_MS), K_MSEC(LOAD_PERIOD_MS));
	}
	atomic_set(&measuring, 1);
	uint32_t window_start = k_cycle_get_32();
	k_timer_start(&release_timer, K_MSEC(PERIOD_MS), K_MSEC(PERIOD_MS));
	int ret = k_sem_take(&jobs_done, K_SECONDS(8));
	uint32_t window_cycles = k_cycle_get_32() - window_start;
	atomic_clear(&measuring);
	k_timer_stop(&release_timer);
	k_timer_stop(&load_timer);
	atomic_clear(&load_running);
	atomic_clear(&bio_running);
	/* Drain workers before touching their data or starting another condition. */
	if (load) {
		k_sem_give(&load_tick);
		if (k_sem_take(&load_done, K_SECONDS(2)) != 0) { return -ETIMEDOUT; }
	}
	if (mode != IDLE && k_sem_take(&bio_done, K_SECONDS(14)) != 0) {
		return -ETIMEDOUT;
	}
	if (ret != 0) { return ret; } /* Never read records from a still-running victim. */
	meta(sh, run, cell, "window_cycles", window_cycles, clock_hz);
	meta(sh, run, cell, "job_counts", atomic_get(&timer_count), atomic_get(&dropped));
	meta(sh, run, cell, "processed_jobs", atomic_get(&processed_jobs), atomic_get(&receive_errors));
	meta(sh, run, cell, "background_counts", atomic_get(&load_releases), atomic_get(&load_executed));
	meta(sh, run, cell, "bio_counts", bio_started, bio_completed);
	meta(sh, run, cell, "bio_window_completed", bio_in_window, atomic_get(&bio_error));
	if (atomic_get(&timer_count) != JOBS || atomic_get(&processed_jobs) != JOBS ||
	    atomic_get(&dropped) != 0 || atomic_get(&receive_errors) != 0) {
		/* Do not publish stale/partial sample slots as a complete new window. */
		return -EIO;
	}
	for (unsigned int i = 0; i < JOBS; ++i) {
		/* Unsigned 32-bit cycles; parser unwraps within the bounded window. */
		shell_print(sh, "G3,J,%u,%u,%u,%u,%u,%u", run, cell, i,
			    samples[i].release, samples[i].start, samples[i].finish);
	}
	bool ok = atomic_get(&timer_count) == JOBS && atomic_get(&dropped) == 0 &&
		atomic_get(&bio_error) == 0 &&
		(mode == IDLE || (bio_started == bio_completed && bio_in_window > 0U));
	return ok ? 0 : -EIO;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	struct biometric_capabilities caps = {0};
	if (!device_is_ready(bio)) { return -ENODEV; }
	int ret = biometric_get_capabilities(bio, &caps);
	if (ret) { return ret; }
	shell_print(sh, "BASE GT5 T3 v1.0; device %s; no samples; no database changes.", bio->name);
	meta(sh, 0, 0, "caps", caps.supported_modalities, caps.async_operations);
	meta(sh, 0, 0, "clock", sys_clock_hw_cycles_per_sec(), CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	meta(sh, 0, 0, "priorities", BIO_PRIORITY, LOAD_PRIORITY);
	meta(sh, 0, 0, "jobs_period_ms", JOBS, PERIOD_MS);
	return 0;
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	char *end;
	errno = 0;
	unsigned long parsed = strtoul(argv[1], &end, 10);
	if (errno || *argv[1] < '0' || *argv[1] > '9' || *end != '\0' ||
	    parsed == 0 || parsed > 1000000UL || parsed <= last_run) {
		return -EINVAL;
	}
	unsigned int run = (unsigned int)parsed;
	if (k_mutex_lock(&runner_lock, K_NO_WAIT) != 0) { return -EBUSY; }
	last_run = run;
	int ret = halted ? -EBUSY : 0;
	struct biometric_capabilities caps = {0};
	if (!ret && !device_is_ready(bio)) { ret = -ENODEV; }
	if (!ret) { ret = biometric_get_capabilities(bio, &caps); }
	if (!ret && (caps.supported_modalities != BIOMETRIC_MODALITY_FINGERPRINT ||
		     caps.async_operations != 0)) { ret = -ENOTSUP; }
	clock_hz = sys_clock_hw_cycles_per_sec();
	/* Every recorded window must be shorter than one 32-bit counter revolution. */
	if (!ret && (clock_hz == 0U || (uint64_t)clock_hz * 10U >= UINT32_MAX)) {
		ret = -ERANGE;
	}
	if (!ret) { ret = calibrate(200U, &victim_iters, &victim_cal_us); }
	if (!ret) { ret = calibrate(3500U, &load_iters, &load_cal_us); }
	meta(sh, run, 0, "version", 1, 0);
	meta(sh, run, 0, "caps", caps.supported_modalities, caps.async_operations);
	meta(sh, run, 0, "clock", clock_hz, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	meta(sh, run, 0, "period_ticks", k_ms_to_ticks_ceil32(PERIOD_MS),
	     k_ms_to_ticks_ceil32(LOAD_PERIOD_MS));
	meta(sh, run, 0, "jobs_period_ms", JOBS, PERIOD_MS);
	meta(sh, run, 0, "victim_calibration", victim_iters, victim_cal_us);
	meta(sh, run, 0, "load_calibration", load_iters, load_cal_us);
	meta(sh, run, 0, "priorities", BIO_PRIORITY, LOAD_PRIORITY);
	verdict(sh, run, 0, "setup", ret == 0, ret);
	if (ret == 0) {
		unsigned int cell = 0;
		for (unsigned int group = 0; group < 4 && ret == 0; ++group) {
			int priority = group < 2 ? 2 : 8;
			unsigned int load = group % 2U ? 50 : 0;
			for (unsigned int m = 0; m < 2 && ret == 0; ++m) {
				enum mode mode = (enum mode)((m + run - 1U + group) % 2U);
				ret = do_cell(sh, run, ++cell, mode, priority, load);
				verdict(sh, run, cell, "cell", ret == 0, ret);
			}
		}
	}
	if (ret) {
		halted = true;
		atomic_clear(&measuring); atomic_clear(&bio_running); atomic_clear(&load_running);
		k_timer_stop(&release_timer); k_timer_stop(&load_timer);
		shell_error(sh, "Halted: retain this log and inspect before reset. No further trials.");
	}
	verdict(sh, run, 0, "suite", ret == 0, ret);
	k_mutex_unlock(&runner_lock);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(t3_commands,
	SHELL_CMD_ARG(info, NULL, "Show configuration.", cmd_info, 1, 0),
	SHELL_CMD_ARG(run, NULL, "Run all 8 conditions: t3 run <positive-run-id>.", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(t3, &t3_commands, "GT5 scheduling evaluation", NULL);

int main(void)
{
	return 0;
}
