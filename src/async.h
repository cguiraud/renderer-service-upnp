/*
 * renderer-service-upnp
 *
 * Copyright (C) 2012 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Mark Ryan <mark.d.ryan@intel.com>
 *
 */

#ifndef RSU_ASYNC_H__
#define RSU_ASYNC_H__

#include <libgupnp/gupnp-control-point.h>

#include "device.h"
#include "task.h"
#include "upnp.h"

typedef struct rsu_async_cb_data_t_ rsu_async_cb_data_t;
struct rsu_async_cb_data_t_ {
	rsu_task_type_t type;
	rsu_task_t *task;
	rsu_upnp_task_complete_t cb;
	void *user_data;
	GVariant *result;
	GError *error;
	GUPnPServiceProxyAction *action;
	GUPnPServiceProxy *proxy;
	GCancellable *cancellable;
	gulong cancel_id;
	gpointer private;
	GDestroyNotify free_private;
	rsu_device_t *device;
};

rsu_async_cb_data_t *rsu_async_cb_data_new(rsu_task_t *task,
					   rsu_upnp_task_complete_t cb,
					   void *user_data,
					   gpointer private,
					   GDestroyNotify free_private,
					   rsu_device_t *device);

gboolean rsu_async_complete_task(gpointer user_data);
void rsu_async_task_cancelled(GCancellable *cancellable, gpointer user_data);
void rsu_async_task_lost_object(gpointer user_data);

#endif
