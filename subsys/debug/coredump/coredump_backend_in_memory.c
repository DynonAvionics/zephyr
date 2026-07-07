/*
 * Copyright (c) 2025 Bang & Olufsen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/debug/coredump.h>

#include "coredump_internal.h"

LOG_MODULE_REGISTER(coredump, CONFIG_DEBUG_COREDUMP_LOG_LEVEL);

/*
 * TIL-101 (Dynon): keep the hardware watchdog fed while the coredump is captured.
 *
 * z_fatal_error() locks interrupts and runs coredump() before the application's fatal
 * handler, and this backend's start()/buffer_output() below (the pre-LOG_PANIC log drain
 * plus the dump copy) can exceed a tight hardware-watchdog window on busy control-loop
 * boards. Nothing else feeds the watchdog during that time, so feed it here. Feeding is a
 * bare register write, safe with interrupts locked.
 *
 * This is a weak *declaration* of an optional hook whose strong definition is provided by
 * the Dynon system_monitor watchdog wrapper (Watchdog.cpp). A weak declaration (rather than
 * a local weak no-op definition) guarantees the strong override is what gets called when
 * present, and the NULL check below turns it into a no-op when it isn't linked.
 */
extern void watchdog_feed_from_panic(void) __attribute__((weak));

static inline void coredump_feed_watchdog(void)
{
	if (watchdog_feed_from_panic != NULL) {
		watchdog_feed_from_panic();
	}
}

#define IN_MEMORY_CANARY_SIZE 4
#define IN_MEMORY_COREDUMP_SIZE_RECORD sizeof(size_t)

/**
 * In-memory coredump space is arranged that way:
 * CANARY+ Recorded coredump size + Coredump + Left space (if any) + CANARY
 */
#define IN_MEMORY_SPACE CONFIG_DEBUG_COREDUMP_BACKEND_IN_MEMORY_SIZE + \
	(IN_MEMORY_CANARY_SIZE * 2) + IN_MEMORY_COREDUMP_SIZE_RECORD
#define IN_MEMORY_START IN_MEMORY_CANARY_SIZE + IN_MEMORY_COREDUMP_SIZE_RECORD
#define IN_MEMORY_END IN_MEMORY_SPACE - IN_MEMORY_CANARY_SIZE

static const uint8_t in_memory_canary[IN_MEMORY_CANARY_SIZE] = {
	0xDE, 0xB0, 0xDE, 0xB0
};

static uint8_t __noinit_named(_in_memory_coredump)
	__aligned(4) in_memory_coredump[IN_MEMORY_SPACE];

static size_t *coredump_size =
	(size_t *)&in_memory_coredump[IN_MEMORY_CANARY_SIZE];
static uint8_t *cur_ptr;


static inline void in_memory_invalidate(void)
{
	memset(in_memory_coredump, 0, IN_MEMORY_CANARY_SIZE);
	memset(&in_memory_coredump[IN_MEMORY_END], 0, IN_MEMORY_CANARY_SIZE);
	*coredump_size = 0;
	cur_ptr = NULL;
}
static inline void in_memory_erase(void)
{
	LOG_DBG("Erasing in-memory coredump\n");

	in_memory_invalidate();
}

static int in_memory_is_valid(void)
{
	if (!memcmp(in_memory_coredump,
		    in_memory_canary, IN_MEMORY_CANARY_SIZE) &&
	    !memcmp(&in_memory_coredump[IN_MEMORY_END],
		    in_memory_canary, IN_MEMORY_CANARY_SIZE)) {
		return 1;
	}

	return 0;
}

static int in_memory_copy_to(struct coredump_cmd_copy_arg *copy_arg)
{
	LOG_DBG("Copy to: %p offset: %lu length: %lu",
		(void *)copy_arg->buffer, copy_arg->offset,
		(unsigned long)copy_arg->length);

	if (copy_arg->buffer == NULL ||
	    copy_arg->offset >= IN_MEMORY_END ||
	    (copy_arg->length + copy_arg->offset) >= IN_MEMORY_END) {
		return -EINVAL;
	}

	if (in_memory_is_valid() == 0) {
		return -EIO;
	}

	memcpy(copy_arg->buffer,
	       &in_memory_coredump[IN_MEMORY_START + copy_arg->offset],
	       copy_arg->length);

	return 0;
}

static void coredump_in_memory_backend_start(void)
{
	coredump_feed_watchdog();

	/*
	 * TIL-101 (Dynon): capture-first ordering.
	 *
	 * Upstream drained the deferred log backlog here with `while (LOG_PROCESS())` and
	 * then called LOG_PANIC(), all *before* a single dump byte was written. LOG_PANIC()
	 * puts every log backend into panic mode and synchronously flushes the whole backlog
	 * (plus the arch fault/register dump) out the console. On a chatty board with a tight
	 * hardware watchdog that flush can outlast the watchdog window and reset the MCU
	 * *before* the dump reaches RAM -- so no coredump is captured at all.
	 *
	 * This backend stores the dump in __noinit RAM (see buffer_output() below) and it is
	 * retrieved later over MCUMgr, never read back from the console. The pre-dump console
	 * flush is therefore pure cost with no benefit, so we drop it and go straight to
	 * writing the dump. If console output of the fault is wanted, it can happen afterward
	 * in the application fatal handler, once the dump is safely in RAM.
	 */

	in_memory_erase();

	memcpy(in_memory_coredump, in_memory_canary, IN_MEMORY_CANARY_SIZE);
	cur_ptr = &in_memory_coredump[IN_MEMORY_START];

	*coredump_size = 0;
}

static void coredump_in_memory_backend_end(void)
{
	memcpy(&in_memory_coredump[IN_MEMORY_END],
	       in_memory_canary, IN_MEMORY_CANARY_SIZE);

	*coredump_size = cur_ptr - &in_memory_coredump[IN_MEMORY_START];

	LOG_ERR(COREDUMP_PREFIX_STR COREDUMP_END_STR);
}

static void coredump_in_memory_backend_buffer_output(uint8_t *buf,
						     size_t buflen)
{
	int space;

	/* Called once per dump chunk; keep the watchdog fed across the whole dump. */
	coredump_feed_watchdog();

	LOG_DBG("Output buffer size %lu", (unsigned long)buflen);

	if (cur_ptr == &in_memory_coredump[IN_MEMORY_END]) {
		/* Once full, we silently ignore the request */
		return;
	}

	space = &in_memory_coredump[IN_MEMORY_END] - cur_ptr;
	if (buflen < space) {
		space = buflen;
	}

	memcpy(cur_ptr, buf, space);
	cur_ptr += space;
}

static int coredump_in_memory_backend_query(enum coredump_query_id query_id,
					    void *arg)
{
	switch (query_id) {
	case COREDUMP_QUERY_GET_ERROR:
		break;
	case COREDUMP_QUERY_HAS_STORED_DUMP:
		return in_memory_is_valid();
	case COREDUMP_QUERY_GET_STORED_DUMP_SIZE:
		if (in_memory_is_valid() == 1) {
			return *coredump_size;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int coredump_in_memory_backend_cmd(enum coredump_cmd_id cmd_id,
					  void *arg)
{
	switch (cmd_id) {
	case COREDUMP_CMD_CLEAR_ERROR:
		break;
	case COREDUMP_CMD_VERIFY_STORED_DUMP:
		return in_memory_is_valid();
	case COREDUMP_CMD_ERASE_STORED_DUMP:
		in_memory_erase();
		break;
	case COREDUMP_CMD_COPY_STORED_DUMP:
		if (arg == NULL) {
			return -EINVAL;
		}
		return in_memory_copy_to((struct coredump_cmd_copy_arg *)arg);
	case COREDUMP_CMD_INVALIDATE_STORED_DUMP:
		in_memory_invalidate();
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

struct coredump_backend_api coredump_backend_in_memory = {
	.start = coredump_in_memory_backend_start,
	.end = coredump_in_memory_backend_end,
	.buffer_output = coredump_in_memory_backend_buffer_output,
	.query = coredump_in_memory_backend_query,
	.cmd = coredump_in_memory_backend_cmd,
};
