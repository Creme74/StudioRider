/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The StudioWire authors
 *
 * Included by civetweb.c (see CMakeLists.txt: MG_EXTERNAL_FUNCTION_*). */
/* StudioWire: no access log (no file is ever written). */
static void
log_access(const struct mg_connection *conn)
{
	(void)conn;
}
