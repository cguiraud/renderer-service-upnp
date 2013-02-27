/*
 * renderer-service-upnp
 *
 * Copyright (C) 2012-2013 Intel Corporation. All rights reserved.
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

#include <string.h>

#include <libgssdp/gssdp-resource-browser.h>
#include <libgupnp/gupnp-context-manager.h>
#include <libgupnp/gupnp-error.h>

#include "async.h"
#include "device.h"
#include "error.h"
#include "host-service.h"
#include "log.h"
#include "prop-defs.h"
#include "upnp.h"
#include "service-task.h"

struct rsu_upnp_t_ {
	GDBusConnection *connection;
	rsu_interface_info_t *interface_info;
	rsu_upnp_callback_t found_server;
	rsu_upnp_callback_t lost_server;
	GUPnPContextManager *context_manager;
	void *user_data;
	GHashTable *server_udn_map;
	GHashTable *server_uc_map;
	guint counter;
	rsu_host_service_t *host_service;
};

/* Private structure used in service task */
typedef struct prv_device_new_ct_t_ prv_device_new_ct_t;
struct prv_device_new_ct_t_ {
	rsu_upnp_t *upnp;
	char *udn;
	rsu_device_t *device;
	const rsu_task_queue_key_t *queue_id;
};

static void prv_device_new_free(prv_device_new_ct_t *priv_t)
{
	if (priv_t) {
		g_free(priv_t->udn);
		g_free(priv_t);
	}
}

static void prv_device_chain_end(gboolean cancelled, gpointer data)
{
	rsu_device_t *device;
	prv_device_new_ct_t *priv_t = (prv_device_new_ct_t *)data;

	RSU_LOG_DEBUG("Enter");

	device = priv_t->device;

	if (cancelled)
		goto on_clear;

	RSU_LOG_DEBUG("Notify new server available: %s", device->path);
	g_hash_table_insert(priv_t->upnp->server_udn_map, g_strdup(priv_t->udn),
			    device);
	priv_t->upnp->found_server(device->path);

on_clear:

	g_hash_table_remove(priv_t->upnp->server_uc_map, priv_t->udn);
	prv_device_new_free(priv_t);

	if (cancelled)
		rsu_device_delete(device);

	RSU_LOG_DEBUG("Exit");
	RSU_LOG_DEBUG_NL();
}

static void prv_server_available_cb(GUPnPControlPoint *cp,
				    GUPnPDeviceProxy *proxy,
				    gpointer user_data)
{
	rsu_upnp_t *upnp = user_data;
	const char *udn;
	rsu_device_t *device;
	const gchar *ip_address;
	rsu_device_context_t *context;
	const rsu_task_queue_key_t *queue_id;
	unsigned int i;
	prv_device_new_ct_t *priv_t;

	RSU_LOG_DEBUG("Enter");

	udn = gupnp_device_info_get_udn((GUPnPDeviceInfo *)proxy);

	if (!udn)
		goto on_error;

	ip_address = gupnp_context_get_host_ip(
		gupnp_control_point_get_context(cp));

	RSU_LOG_DEBUG("UDN %s", udn);
	RSU_LOG_DEBUG("IP Address %s", ip_address);

	device = g_hash_table_lookup(upnp->server_udn_map, udn);

	if (!device) {
		priv_t = g_hash_table_lookup(upnp->server_uc_map, udn);

		if (priv_t)
			device = priv_t->device;
	}

	if (!device) {
		RSU_LOG_DEBUG("Device not found. Adding");

		priv_t = g_new0(prv_device_new_ct_t, 1);

		queue_id = rsu_task_processor_add_queue(
				rsu_renderer_service_get_task_processor(),
				rsu_service_task_create_source(),
				RSU_SINK,
				RSU_TASK_QUEUE_FLAG_AUTO_REMOVE,
				rsu_service_task_process_cb,
				rsu_service_task_cancel_cb,
				rsu_service_task_delete_cb);
		rsu_task_queue_set_finally(queue_id, prv_device_chain_end);
		rsu_task_queue_set_user_data(queue_id, priv_t);

		device = rsu_device_new(upnp->connection, proxy, ip_address,
					upnp->counter,
					upnp->interface_info,
					queue_id);

		upnp->counter++;

		priv_t->upnp = upnp;
		priv_t->udn = g_strdup(udn);
		priv_t->queue_id = queue_id;
		priv_t->device = device;

		g_hash_table_insert(upnp->server_uc_map, g_strdup(udn), priv_t);

	} else {
		RSU_LOG_DEBUG("Device Found");

		for (i = 0; i < device->contexts->len; ++i) {
			context = g_ptr_array_index(device->contexts, i);
			if (!strcmp(context->ip_address, ip_address))
				break;
		}

		if (i == device->contexts->len) {
			RSU_LOG_DEBUG("Adding Context");
			rsu_device_append_new_context(device, ip_address,
						      proxy);
		}
	}

on_error:

	RSU_LOG_DEBUG("Exit");
	RSU_LOG_DEBUG_NL();

	return;
}

static gboolean prv_subscribe_to_service_changes(gpointer user_data)
{
	rsu_device_t *device = user_data;

	device->timeout_id = 0;
	rsu_device_subscribe_to_service_changes(device);

	return FALSE;
}

static void prv_server_unavailable_cb(GUPnPControlPoint *cp,
				      GUPnPDeviceProxy *proxy,
				      gpointer user_data)
{
	rsu_upnp_t *upnp = user_data;
	const char *udn;
	rsu_device_t *device;
	const gchar *ip_address;
	unsigned int i;
	rsu_device_context_t *context;
	gboolean subscribed;
	gboolean under_construction = FALSE;
	prv_device_new_ct_t *priv_t;

	RSU_LOG_DEBUG("Enter");

	udn = gupnp_device_info_get_udn((GUPnPDeviceInfo *)proxy);

	if (!udn)
		goto on_error;

	ip_address = gupnp_context_get_host_ip(
		gupnp_control_point_get_context(cp));

	RSU_LOG_DEBUG("UDN %s", udn);
	RSU_LOG_DEBUG("IP Address %s", ip_address);

	device = g_hash_table_lookup(upnp->server_udn_map, udn);

	if (!device) {
		priv_t = g_hash_table_lookup(upnp->server_uc_map, udn);

		if (priv_t) {
			device = priv_t->device;
			under_construction = TRUE;
		}
	}

	if (!device) {
		RSU_LOG_WARNING("Device not found. Ignoring");
		goto on_error;
	}

	for (i = 0; i < device->contexts->len; ++i) {
		context = g_ptr_array_index(device->contexts, i);
		if (!strcmp(context->ip_address, ip_address))
			break;
	}

	if (i < device->contexts->len) {
		subscribed = (context->subscribed_av || context->subscribed_cm);

		(void) g_ptr_array_remove_index(device->contexts, i);

		if (device->contexts->len == 0) {
			if (!under_construction) {
				RSU_LOG_DEBUG(
				       "Last Context lost. Delete device");

				upnp->lost_server(device->path);
				g_hash_table_remove(upnp->server_udn_map, udn);
			} else {
				RSU_LOG_WARNING(
				       "Device under construction. Cancelling");

				rsu_task_processor_cancel_queue(
							priv_t->queue_id);
			}
		} else if (subscribed && !device->timeout_id) {
			RSU_LOG_DEBUG("Subscribe on new context");

			device->timeout_id = g_timeout_add_seconds(1,
					prv_subscribe_to_service_changes,
					device);
		}
	}

on_error:

	return;
}

static void prv_on_context_available(GUPnPContextManager *context_manager,
				     GUPnPContext *context,
				     gpointer user_data)
{
	rsu_upnp_t *upnp = user_data;
	GUPnPControlPoint *cp;

	cp = gupnp_control_point_new(
		context,
		"urn:schemas-upnp-org:device:MediaRenderer:1");

	g_signal_connect(cp, "device-proxy-available",
			 G_CALLBACK(prv_server_available_cb), upnp);

	g_signal_connect(cp, "device-proxy-unavailable",
			 G_CALLBACK(prv_server_unavailable_cb), upnp);

	gssdp_resource_browser_set_active(GSSDP_RESOURCE_BROWSER(cp), TRUE);
	gupnp_context_manager_manage_control_point(upnp->context_manager, cp);
	g_object_unref(cp);
}

rsu_upnp_t *rsu_upnp_new(GDBusConnection *connection,
			 rsu_interface_info_t *interface_info,
			 rsu_upnp_callback_t found_server,
			 rsu_upnp_callback_t lost_server)
{
	rsu_upnp_t *upnp = g_new0(rsu_upnp_t, 1);

	upnp->connection = connection;
	upnp->interface_info = interface_info;
	upnp->found_server = found_server;
	upnp->lost_server = lost_server;

	upnp->server_udn_map = g_hash_table_new_full(g_str_hash, g_str_equal,
						     g_free,
						     rsu_device_delete);

	upnp->server_uc_map = g_hash_table_new_full(g_str_hash, g_str_equal,
						    g_free, NULL);

	upnp->context_manager = gupnp_context_manager_create(0);

	g_signal_connect(upnp->context_manager, "context-available",
			 G_CALLBACK(prv_on_context_available),
			 upnp);

	rsu_host_service_new(&upnp->host_service);

	return upnp;
}

void rsu_upnp_delete(rsu_upnp_t *upnp)
{
	if (upnp) {
		rsu_host_service_delete(upnp->host_service);
		g_object_unref(upnp->context_manager);
		g_hash_table_unref(upnp->server_udn_map);
		g_hash_table_unref(upnp->server_uc_map);
		g_free(upnp->interface_info);
		g_free(upnp);
	}
}

GVariant *rsu_upnp_get_server_ids(rsu_upnp_t *upnp)
{
	GVariantBuilder vb;
	GHashTableIter iter;
	gpointer value;
	rsu_device_t *device;

	RSU_LOG_DEBUG("Enter");

	g_variant_builder_init(&vb, G_VARIANT_TYPE("as"));
	g_hash_table_iter_init(&iter, upnp->server_udn_map);

	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		device = value;
		g_variant_builder_add(&vb, "s", device->path);
	}

	RSU_LOG_DEBUG("Exit");

	return g_variant_ref_sink(g_variant_builder_end(&vb));
}

GHashTable *rsu_upnp_get_server_udn_map(rsu_upnp_t *upnp)
{
	return upnp->server_udn_map;
}


void rsu_upnp_set_prop(rsu_upnp_t *upnp, rsu_task_t *task,
		       rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_set_prop(device, task, cb);
	}
}

void rsu_upnp_get_prop(rsu_upnp_t *upnp, rsu_task_t *task,
		       rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	RSU_LOG_DEBUG("Path: %s", task->path);
	RSU_LOG_DEBUG("Interface %s", task->ut.get_prop.interface_name);
	RSU_LOG_DEBUG("Prop.%s", task->ut.get_prop.prop_name);

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		RSU_LOG_WARNING("Cannot locate device");

		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_get_prop(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_get_all_props(rsu_upnp_t *upnp, rsu_task_t *task,
			    rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	RSU_LOG_DEBUG("Path: %s", task->path);
	RSU_LOG_DEBUG("Interface %s", task->ut.get_prop.interface_name);

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_get_all_props(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_play(rsu_upnp_t *upnp, rsu_task_t *task,
		   rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_play(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_pause(rsu_upnp_t *upnp, rsu_task_t *task,
		    rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_pause(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_play_pause(rsu_upnp_t *upnp, rsu_task_t *task,
			 rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_play_pause(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_stop(rsu_upnp_t *upnp, rsu_task_t *task,
		   rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_stop(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_next(rsu_upnp_t *upnp, rsu_task_t *task,
		   rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_next(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_previous(rsu_upnp_t *upnp, rsu_task_t *task,
		       rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_previous(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_open_uri(rsu_upnp_t *upnp, rsu_task_t *task,
		       rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_open_uri(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_seek(rsu_upnp_t *upnp, rsu_task_t *task,
		   rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_seek(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_set_position(rsu_upnp_t *upnp, rsu_task_t *task,
			   rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_set_position(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_goto_track(rsu_upnp_t *upnp, rsu_task_t *task,
			 rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_goto_track(device, task, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_host_uri(rsu_upnp_t *upnp, rsu_task_t *task,
		       rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_host_uri(device, task, upnp->host_service, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_remove_uri(rsu_upnp_t *upnp, rsu_task_t *task,
			 rsu_upnp_task_complete_t cb)
{
	rsu_device_t *device;
	rsu_async_task_t *cb_data = (rsu_async_task_t *)task;

	RSU_LOG_DEBUG("Enter");

	device = rsu_device_from_path(task->path, upnp->server_udn_map);

	if (!device) {
		cb_data->cb = cb;
		cb_data->error = g_error_new(RSU_ERROR,
					     RSU_ERROR_OBJECT_NOT_FOUND,
					     "Cannot locate a device"
					     " for the specified "
					     "object");
		(void) g_idle_add(rsu_async_task_complete, cb_data);
	} else {
		rsu_device_remove_uri(device, task, upnp->host_service, cb);
	}

	RSU_LOG_DEBUG("Exit");
}

void rsu_upnp_lost_client(rsu_upnp_t *upnp, const gchar *client_name)
{
	rsu_host_service_lost_client(upnp->host_service, client_name);
}

void rsu_upnp_unsubscribe(rsu_upnp_t *upnp)
{
	GHashTableIter iter;
	gpointer value;
	rsu_device_t *device;

	RSU_LOG_DEBUG("Enter");

	g_hash_table_iter_init(&iter, upnp->server_udn_map);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		device = value;
		rsu_device_unsubscribe(device);
	}

	RSU_LOG_DEBUG("Exit");
}
