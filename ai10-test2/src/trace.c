/* SPDX-License-Identifier: Apache-2.0 */
#include "trace.h"

struct t2_verdict t2_evaluate(const struct t2_event *events, size_t count,
			    unsigned int dropped, enum t2_expect expected,
			    int timeout_status, int cancel_status)
{
	struct t2_verdict v = { .lifecycle = dropped == 0U };
	unsigned int matches = 0U, misses = 0U, enrolled = 0U, errors = 0U;
	int error_status = 0;
	bool terminal_seen = false;

	for (size_t i = 0U; i < count; i++) {
		const struct t2_event *e = &events[i];

		if (terminal_seen || (i > 0U && e->ms < events[i - 1U].ms)) {
			v.lifecycle = false;
		}
		switch (e->kind) {
		case T2_MATCH:
			matches++;
			if (e->status != 0 || errors != 0U) { v.lifecycle = false; }
			break;
		case T2_NO_MATCH:
			misses++;
			if (e->status >= 0 || errors != 0U) { v.lifecycle = false; }
			break;
		case T2_ENROLLED:
			enrolled++;
			if (e->status != 0 || errors != 0U) { v.lifecycle = false; }
			break;
		case T2_ERROR:
			errors++;
			error_status = e->status;
			if (e->status >= 0) { v.lifecycle = false; }
			break;
		case T2_STOPPED:
			v.stops++;
			v.terminal_status = e->status;
			terminal_seen = true;
			break;
		default:
			v.lifecycle = false;
			break;
		}
	}
	if (v.stops != 1U || errors > 1U || v.terminal_status > 0) {
		v.lifecycle = false;
	}
	if ((errors != 0U && error_status != v.terminal_status) ||
	    (errors == 0U && v.terminal_status != 0 && v.terminal_status != cancel_status)) {
		v.lifecycle = false;
	}

	switch (expected) {
	case T2_ENROLL_TIMEOUT:
		v.outcome = count == 2U && errors == 1U &&
			    error_status == timeout_status && v.terminal_status == timeout_status;
		break;
	case T2_ONCE_NO_SAMPLE:
		v.outcome = count == 2U && misses == 1U && errors == 0U &&
			    v.terminal_status == 0;
		break;
	case T2_CANCEL_ENROLL:
		v.outcome = count == 1U && v.terminal_status == cancel_status;
		break;
	case T2_CANCEL_IDENTIFY:
		v.outcome = matches == 0U && enrolled == 0U && errors == 0U &&
			    v.terminal_status == cancel_status;
		break;
	}
	v.outcome = v.outcome && v.lifecycle;
	return v;
}
