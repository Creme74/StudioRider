/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The StudioRider authors
 *
 * Included by civetweb.c (see CMakeLists.txt: MG_EXTERNAL_FUNCTION_*). */
/* StudioRider: no access log (no file is ever written). */
static void
log_access(const struct mg_connection *conn)
{
	(void)conn;
}
