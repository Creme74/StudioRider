/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The StudioWire authors
 *
 * Included by civetweb.c (see CMakeLists.txt: MG_EXTERNAL_FUNCTION_*). */
/* StudioWire: civetweb is built without a file system.
 * Internal error messages are ignored (no log file). */
static void
mg_cry_internal_impl(const struct mg_connection *conn,
                     const char *func,
                     unsigned line,
                     const char *fmt,
                     va_list ap)
{
	(void)conn;
	(void)func;
	(void)line;
	(void)fmt;
	(void)ap;
}
