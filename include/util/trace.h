#ifndef UTIL_TRACE_H
#define UTIL_TRACE_H

#include <stdint.h>
#include <wlr/util/log.h>

void wlr_trace(const char *format, ...) _WLR_ATTRIB_PRINTF(1, 2);
void wlr_vtrace(const char *format, va_list args) _WLR_ATTRIB_PRINTF(1, 0);

struct wlr_trace_ctx {
	uint32_t seq;
};

void wlr_trace_begin_ctx(struct wlr_trace_ctx *ctx, const char *format, ...) _WLR_ATTRIB_PRINTF(2, 3);
void wlr_trace_end_ctx(struct wlr_trace_ctx *ctx, const char *format, ...) _WLR_ATTRIB_PRINTF(2, 3);

#endif
