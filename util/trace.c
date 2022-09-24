#include <inttypes.h>
#include <stdio.h>
#include "util/trace.h"

static bool trace_initialized = false;
static FILE *trace_file = NULL;
static uint32_t prev_ctx = 0;

void wlr_vtrace(const char *fmt, va_list args) {
	if (!trace_initialized) {
		trace_initialized = true;
		trace_file = fopen("/sys/kernel/tracing/trace_marker", "w");
		if (trace_file != NULL) {
			wlr_log(WLR_INFO, "Kernel tracing is enabled");
		}
	}

	if (trace_file == NULL) {
		return;
	}

	vfprintf(trace_file, fmt, args);
	fprintf(trace_file, "\n");
	fflush(trace_file);
}

void wlr_trace(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	wlr_vtrace(fmt, args);
	va_end(args);
}

void wlr_trace_begin_ctx(struct wlr_trace_ctx *ctx, const char *fmt, ...) {
	*ctx = (struct wlr_trace_ctx){
		.seq = prev_ctx++,
	};

	static char new_fmt[512];
	snprintf(new_fmt, sizeof(new_fmt), "%s (begin_ctx=%"PRIu32")", fmt, ctx->seq);

	va_list args;
	va_start(args, fmt);
	wlr_vtrace(new_fmt, args);
	va_end(args);
}

void wlr_trace_end_ctx(struct wlr_trace_ctx *ctx, const char *fmt, ...) {
	static char new_fmt[512];
	snprintf(new_fmt, sizeof(new_fmt), "%s (end_ctx=%"PRIu32")", fmt, ctx->seq);

	va_list args;
	va_start(args, fmt);
	wlr_vtrace(new_fmt, args);
	va_end(args);

	ctx->seq = 0;
}
