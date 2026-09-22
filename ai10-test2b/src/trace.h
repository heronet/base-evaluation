/* SPDX-License-Identifier: Apache-2.0 */
#ifndef T2_TRACE_H
#define T2_TRACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum t2_kind { T2_MATCH, T2_NO_MATCH, T2_ENROLLED, T2_ERROR, T2_STOPPED, T2_UNKNOWN };
enum t2_expect { T2_ENROLL_TIMEOUT, T2_ONCE_NO_SAMPLE, T2_CANCEL_ENROLL, T2_CANCEL_IDENTIFY };
struct t2_event {
	enum t2_kind kind;
	int status;
	uint16_t id;
	uint32_t modality;
	int64_t ms;
};
struct t2_verdict {
	bool lifecycle;
	bool outcome;
	unsigned int stops;
	int terminal_status;
};
struct t2_verdict t2_evaluate(const struct t2_event *events, size_t count,
			    unsigned int dropped, enum t2_expect expected,
			    int timeout_status, int cancel_status);
#endif
