/* libnih
 *
 * dbus.c - D-Bus bindings
 *
 * Copyright © 2008 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <dbus/dbus.h>

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/timer.h>
#include <nih/io.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>
#include <nih/errors.h>

#include "dbus.h"


/* Prototypes for static functions */
static dbus_bool_t       nih_dbus_add_watch         (DBusWatch *watch,
						     void *data);
static void              nih_dbus_remove_watch      (DBusWatch *watch,
						     void *data);
static void              nih_dbus_watch_toggled     (DBusWatch *watch,
						     void *data);
static void              nih_dbus_watcher           (DBusWatch *watch,
						     NihIoWatch *io_watch,
						     NihIoEvents events);
static dbus_bool_t       nih_dbus_add_timeout       (DBusTimeout *timeout,
						     void *data);
static void              nih_dbus_remove_timeout    (DBusTimeout *timeout,
						     void *data);
static void              nih_dbus_timeout_toggled   (DBusTimeout *timeout,
						     void *data);
static void              nih_dbus_timer             (DBusTimeout *timeout,
						     NihTimer *timer);
static void              nih_dbus_wakeup_main       (void *data);
static void              nih_dbus_callback          (DBusConnection *conn,
						     NihMainLoopFunc *loop);
static DBusHandlerResult nih_dbus_connection_disconnected (DBusConnection *conn,
							   DBusMessage *message,
							   NihDBusDisconnectHandler handler);
static void              nih_dbus_new_connection    (DBusServer *server,
						     DBusConnection *conn,
						     void *data);
static int               nih_dbus_object_destroy    (NihDBusObject *object);
static void              nih_dbus_object_unregister (DBusConnection *conn,
						     NihDBusObject *object);
static DBusHandlerResult nih_dbus_object_message    (DBusConnection *conn,
						     DBusMessage *message,
						     NihDBusObject *object);
static DBusHandlerResult nih_dbus_object_introspect (DBusConnection *conn,
						     DBusMessage *message,
						     NihDBusObject *object);


/**
 * nih_dbus_object_vtable:
 *
 * Table of functions for handling D-Bus objects.
 **/
const static DBusObjectPathVTable nih_dbus_object_vtable = {
	(DBusObjectPathUnregisterFunction)nih_dbus_object_unregister,
	(DBusObjectPathMessageFunction)nih_dbus_object_message,
	NULL
};

/**
 * main_loop_slot:
 *
 * Slot we use to store the main loop function in the connection.
 **/
static dbus_int32_t main_loop_slot = -1;

/**
 * connect_handler_slot:
 *
 * Slot we use to store the connection handler in the server.
 **/
static dbus_int32_t connect_handler_slot = -1;

/**
 * disconnect_handler_slot:
 *
 * Slot we use to store the disconnect handler in the server.
 **/
static dbus_int32_t disconnect_handler_slot = -1;


/**
 * nih_dbus_error_raise:
 * @name: D-Bus name for error,
 * @message: Human-readable error message.
 *
 * Raises an error which includes a D-Bus name so that it may be sent as
 * a reply to a method call, the error type is fixed to NIH_DBUS_ERROR.
 *
 * You may use this in D-Bus handlers and return a negative number to
 * automatically have this error returned as the method reply.  It is also
 * useful when mixing D-Bus and libnih function calls in your own methods
 * to return consistent error forms, in which case pass the name and message
 * members of the DBusError structure before freeing it.
 **/
void
nih_dbus_error_raise (const char *name,
		      const char *message)
{
	NihDBusError *err;

	nih_assert (name != NULL);
	nih_assert (message != NULL);

	NIH_MUST (err = nih_new (NULL, NihDBusError));

	err->error.number = NIH_DBUS_ERROR;
	NIH_MUST (err->name = nih_strdup (err, name));
	NIH_MUST (err->error.message = nih_strdup (err, message));

	nih_error_raise_again (&err->error);
}

/**
 * nih_dbus_error_raise_printf:
 * @name: D-Bus name for error,
 * @format: format string for human-readable message.
 *
 * Raises an error which includes a D-Bus name so that it may be sent as
 * a reply to a method call, the error type is fixed to NIH_DBUS_ERROR.
 *
 * The human-readable message for the error is parsed according to @format,
 * and allocated as a child of the error object so that it is freed.
 *
 * You may use this in D-Bus handlers and return a negative number to
 * automatically have this error returned as the method reply.  It is also
 * useful when mixing D-Bus and libnih function calls in your own methods
 * to return consistent error forms, in which case pass the name and message
 * members of the DBusError structure before freeing it.
 **/
void
nih_dbus_error_raise_printf (const char *name,
			     const char *format,
			     ...)
{
	NihDBusError *err;
	va_list       args;

	nih_assert (name != NULL);
	nih_assert (format != NULL);

	NIH_MUST (err = nih_new (NULL, NihDBusError));

	err->error.number = NIH_DBUS_ERROR;

	NIH_MUST (err->name = nih_strdup (err, name));

	va_start (args, format);
	NIH_MUST (err->error.message = nih_vsprintf (err, format, args));
	va_end (args);

	nih_error_raise_again (&err->error);
}


/**
 * nih_dbus_connect:
 * @address: address of D-Bus bus or server,
 * @disconnect_handler: function to call on disconnection.
 *
 * Establishes a connection to the D-Bus bus or server at @address
 * (specified in D-Bus's own address syntax) and sets up the connection
 * within libnih's own main loop so that messages will be received, sent
 * and dispatched automatically.
 *
 * The returned connection object IS NOT allocated with nih_alloc() and
 * is instead allocated and managed by the D-Bus library, it may not be
 * used as a context for other allocations.  Instead you should use
 * D-Bus data slots and free functions to attach other data to this.
 *
 * The connection object is shared and will persist as long as the
 * server maintains the connection, you should not attempt to close or
 * unreference the connection yourself.
 *
 * Returns: new D-Bus connection object or NULL on raised error.
 **/
DBusConnection *
nih_dbus_connect (const char               *address,
		  NihDBusDisconnectHandler  disconnect_handler)
{
	DBusConnection *conn;
	DBusError       error;

	nih_assert (address != NULL);

	dbus_error_init (&error);

	conn = dbus_connection_open (address, &error);
	if (! conn) {
		nih_dbus_error_raise (error.name, error.message);
		dbus_error_free (&error);

		return NULL;
	}

	if (nih_dbus_setup (conn, disconnect_handler) < 0) {
		errno = ENOMEM;
		nih_error_raise_system ();

		dbus_connection_unref (conn);
		return NULL;
	}

	return conn;
}

/**
 * nih_dbus_bus:
 * @bus: D-Bus bus type to connect to,
 * @disconnect_handler: function to call on disconnection.
 *
 * Establishes a connection to the given D-Bus @bus and sets up
 * the connection within libnih's own main loop so that messages will be
 * received, sent and dispatched automatically.
 *
 * Unlike the ordinary D-Bus API, this connection will not cause the exit()
 * function to be called should the bus go away.
 *
 * The returned connection object IS NOT allocated with nih_alloc() and
 * is instead allocated and managed by the D-Bus library, it may not be
 * used as a context for other allocations.  Instead you should use
 * D-Bus data slots and free functions to attach other data to this.
 *
 * The connection object is shared and will persist as long as the
 * server maintains the connection, you should not attempt to close or
 * unreference the connection yourself.
 *
 * Returns: new D-Bus connection object or NULL on raised error.
 **/
DBusConnection *
nih_dbus_bus (DBusBusType              bus,
	      NihDBusDisconnectHandler disconnect_handler)
{
	DBusConnection *conn;
	DBusError       error;

	dbus_error_init (&error);

	conn = dbus_bus_get (bus, &error);
	if (! conn) {
		nih_dbus_error_raise (error.name, error.message);
		dbus_error_free (&error);

		return NULL;
	}

	dbus_connection_set_exit_on_disconnect (conn, FALSE);

	if (nih_dbus_setup (conn, disconnect_handler) < 0) {
		errno = ENOMEM;
		nih_error_raise_system ();

		dbus_connection_unref (conn);
		return NULL;
	}

	return conn;
}

/**
 * nih_dbus_setup:
 * @conn: D-Bus connection to setup,
 * @disconnect_handler: function to call on disconnection.
 *
 * Sets up the given connection @conn so that it may use libnih's own
 * main loop meaning that messages will be received, sent and dispatched
 * automatically.
 *
 * This will also set up a handler for the disconnected signal that will
 * automatically unreference the connection after calling the given
 * @disconnect_handler.
 *
 * Returns: zero on success, negative value on insufficient memory.
 **/
int
nih_dbus_setup (DBusConnection           *conn,
		NihDBusDisconnectHandler  disconnect_handler)
{
	NihMainLoopFunc *loop;

	nih_assert (conn != NULL);

	/* Allocate a data slot for storing the main loop function; if
	 * this is set for the structure, we've already set it up before
	 * and this is being shared so we can skip down to just adding
	 * the new disconnect handler.
	 */
 	if (! dbus_connection_allocate_data_slot (&main_loop_slot))
		return -1;

	if (dbus_connection_get_data (conn, main_loop_slot))
		goto shared;


	/* Add the main loop function and store it in the data slot,
	 * this means it will be automatically freed.
	 */
	loop = nih_main_loop_add_func (NULL, (NihMainLoopCb)nih_dbus_callback,
				       conn);
	if (! loop)
		return -1;

	if (! dbus_connection_set_data (conn, main_loop_slot, loop,
					(DBusFreeFunction)nih_free)) {
		nih_free (loop);
		return -1;
	}

	/* Allow the connection to watch its file descriptors */
	if (! dbus_connection_set_watch_functions (conn,
						   nih_dbus_add_watch,
						   nih_dbus_remove_watch,
						   nih_dbus_watch_toggled,
						   NULL, NULL))
		return -1;

	/* Allow the connection to set up timers */
	if (! dbus_connection_set_timeout_functions (conn,
						     nih_dbus_add_timeout,
						     nih_dbus_remove_timeout,
						     nih_dbus_timeout_toggled,
						     NULL, NULL))
		return -1;

	/* Allow the connection to wake up the main loop */
	dbus_connection_set_wakeup_main_function (conn,
						  nih_dbus_wakeup_main,
						  NULL, NULL);

shared:
	/* Add the filter for the disconnect handler (which may be NULL,
	 * but even then we have to unreference it).
	 */
	if (! dbus_connection_add_filter (
		    conn, (DBusHandleMessageFunction)nih_dbus_connection_disconnected,
		    disconnect_handler, NULL))
		return -1;

	return 0;
}


/**
 * nih_dbus_server:
 * @address: intended address of D-Bus server,
 * @connect_handler: function to call on new connections,
 * @disconnect_handler: function to call on disconnection of connections.
 *
 * Creates a listening D-Bus server at @address (specified in D-Bus's own
 * address syntax) and sets up the server within libnih's own main loop
 * so that socket events will be handled automatically.
 *
 * New connections are accepted if the @connect_handler returns TRUE and
 * they too set up within libnih's own main loop so that messages will be
 * received, sent and dispatched.  If those connections are disconnected,
 * @disconnect_handler will be called for them and they will be
 * automatically unreferenced.
 *
 * The returned server object and any created connection objects ARE NOT
 * allocated with nih_alloc() and are instead allocated and managed by the
 * D-Bus library, they may not be used as a context for other allocations.
 * Instead you should use D-Bus data slots and free functions to attach
 * other data to them.
 *
 * Both the server object and any created connection objects are private,
 * you may close and unreference them when you are finished with them.
 *
 * Returns: new D-Bus server object or NULL on raised error.
 **/
DBusServer *
nih_dbus_server (const char               *address,
		 NihDBusConnectHandler     connect_handler,
		 NihDBusDisconnectHandler  disconnect_handler)
{
	DBusServer *server;
	DBusError   error;

	nih_assert (address != NULL);

	dbus_error_init (&error);

	server = dbus_server_listen (address, &error);
	if (! server) {
		nih_dbus_error_raise (error.name, error.message);
		dbus_error_free (&error);

		return NULL;
	}

	/* Allocate a slot to store the connect handler */
	if (! dbus_server_allocate_data_slot (&connect_handler_slot))
		goto error;

	if (! dbus_server_set_data (server, connect_handler_slot,
				    connect_handler, NULL))
		goto error;

	/* Allocate a slot to store the disconnect handler */
	if (! dbus_server_allocate_data_slot (&disconnect_handler_slot))
		goto error;

	if (! dbus_server_set_data (server, disconnect_handler_slot,
				    disconnect_handler, NULL))
		goto error;

	/* Allow the server to watch its file descriptors */
	if (! dbus_server_set_watch_functions (server,
					       nih_dbus_add_watch,
					       nih_dbus_remove_watch,
					       nih_dbus_watch_toggled,
					       NULL, NULL))
		goto error;

	/* Allow the server to set up timers */
	if (! dbus_server_set_timeout_functions (server,
						 nih_dbus_add_timeout,
						 nih_dbus_remove_timeout,
						 nih_dbus_timeout_toggled,
						 NULL, NULL))
		goto error;

	/* Set the function to be called for new connectoins */
	dbus_server_set_new_connection_function (server,
						 nih_dbus_new_connection,
						 NULL, NULL);

	return server;

error:
	errno = ENOMEM;
	nih_error_raise_system ();

	dbus_server_unref (server);
	return NULL;
}


/**
 * nih_dbus_add_watch:
 * @watch: D-Bus watch to be added,
 * @data: not used.
 *
 * Called by D-Bus to register the given file descriptor @watch in our main
 * loop; we create an NihIoWatch structure for it with events matching the
 * watch's flags - even if the watch is not enabled (in which case we remove
 * it from the watch list).
 *
 * The NihIoWatch is stored in the watch's data member.
 *
 * Returns: TRUE if the watch could be added, FALSE on insufficient memory.
 **/
static dbus_bool_t
nih_dbus_add_watch (DBusWatch *watch,
		    void      *data)
{
	NihIoWatch  *io_watch;
	int          fd, flags;
	NihIoEvents  events = NIH_IO_EXCEPT;

	nih_assert (watch != NULL);
	nih_assert (dbus_watch_get_data (watch) == NULL);

	fd = dbus_watch_get_unix_fd (watch);
	nih_assert (fd >= 0);

	flags = dbus_watch_get_flags (watch);
	if (flags & DBUS_WATCH_READABLE)
		events |= NIH_IO_READ;
	if (flags & DBUS_WATCH_WRITABLE)
		events |= NIH_IO_WRITE;

	io_watch = nih_io_add_watch (NULL, fd, events,
				     (NihIoWatcher)nih_dbus_watcher, watch);
	if (! io_watch)
		return FALSE;

	dbus_watch_set_data (watch, io_watch, (DBusFreeFunction)nih_free);

	if (! dbus_watch_get_enabled (watch))
		nih_list_remove (&io_watch->entry);

	return TRUE;
}

/**
 * nih_dbus_remove_watch:
 * @watch: D-Bus watch to be removed,
 * @data: not used.
 *
 * Called by D-Bus to unregister the given file descriptor @watch from our
 * main loop; we take the NihIoWatch structure from the watch's data member
 * and free it.
 **/
static void
nih_dbus_remove_watch (DBusWatch *watch,
		       void      *data)
{
	NihIoWatch *io_watch;

	nih_assert (watch != NULL);

	io_watch = dbus_watch_get_data (watch);
	nih_assert (io_watch != NULL);

	/* Only remove it from the list, D-Bus will call nih_free for us
	 * when we set the data to NULL.
	 **/
	nih_list_remove (&io_watch->entry);

	dbus_watch_set_data (watch, NULL, NULL);
}

/**
 * nih_dbus_watch_toggled:
 * @watch: D-Bus watch to be toggled,
 * @data: not used.
 *
 * Called by D-Bus because the given file descriptor @watch has been enabled
 * or disabled; we take the NihIoWatch structure from the watch's data member
 * and either add it to or remove it from the watches list.
 **/
static void
nih_dbus_watch_toggled (DBusWatch *watch,
			void      *data)
{
	NihIoWatch *io_watch;

	nih_assert (watch != NULL);

	io_watch = dbus_watch_get_data (watch);
	nih_assert (io_watch != NULL);

	if (dbus_watch_get_enabled (watch)) {
		nih_list_add (nih_io_watches, &io_watch->entry);
	} else {
		nih_list_remove (&io_watch->entry);
	}
}

/**
 * nih_dbus_watcher:
 * @watch: D-Bus watch event occurred for,
 * @io_watch: NihIoWatch for which an event occurred,
 * @events: events that occurred.
 *
 * Called because an event has occurred on @io_watch that we need to pass
 * onto the underlying @watch.
 **/
static void
nih_dbus_watcher (DBusWatch   *watch,
		  NihIoWatch  *io_watch,
		  NihIoEvents  events)
{
	int flags = 0;

	nih_assert (watch != NULL);
	nih_assert (io_watch != NULL);

	if (events & NIH_IO_READ)
		flags |= DBUS_WATCH_READABLE;
	if (events & NIH_IO_WRITE)
		flags |= DBUS_WATCH_WRITABLE;
	if (events & NIH_IO_EXCEPT)
		flags |= DBUS_WATCH_ERROR;

	dbus_watch_handle (watch, flags);
}


/**
 * nih_dbus_add_timeout:
 * @timeout: D-Bus timeout to be added,
 * @data: not used.
 *
 * Called by D-Bus to register the given @timeout in our main loop; we create
 * a periodic NihTimer structure for it with the correct interval even if
 * the timeout is not enabled (in which case we remove it from the timer
 * list).
 *
 * The NihTimer is stored in the timeout's data member.
 *
 * Returns: TRUE if the timeout could be added, FALSE on insufficient memory.
 **/
static dbus_bool_t
nih_dbus_add_timeout (DBusTimeout *timeout,
		      void        *data)
{
	NihTimer *timer;
	int       interval;

	nih_assert (timeout != NULL);
	nih_assert (dbus_timeout_get_data (timeout) == NULL);

	interval = dbus_timeout_get_interval (timeout);

	timer = nih_timer_add_periodic (NULL, (interval - 1) / 1000 + 1,
					(NihTimerCb)nih_dbus_timer, timeout);
	if (! timer)
		return FALSE;

	dbus_timeout_set_data (timeout, timer, (DBusFreeFunction)nih_free);

	if (! dbus_timeout_get_enabled (timeout))
		nih_list_remove (&timer->entry);

	return TRUE;
}

/**
 * nih_dbus_remove_timeout:
 * @timeout: D-Bus timeout to be removed,
 * @data: not used.
 *
 * Called by D-Bus to unregister the given @timeout from our main loop; we
 * take the NihTimer structure from the timeout's data member and free it.
 **/
static void
nih_dbus_remove_timeout (DBusTimeout *timeout,
			 void        *data)
{
	NihTimer *timer;

	nih_assert (timeout != NULL);

	timer = dbus_timeout_get_data (timeout);
	nih_assert (timer != NULL);

	/* Only remove it from the list, D-Bus will call nih_free for us
	 * when we set the data to NULL.
	 */
	nih_list_remove (&timer->entry);

	dbus_timeout_set_data (timeout, NULL, NULL);
}

/**
 * nih_dbus_timeout_toggled:
 * @timeout: D-Bus timeout to be toggled,
 * @data: not used.
 *
 * Called by D-Bus because the @timeout has been enabled or disabled; we
 * take the NihTimer structure from the timeout's data member and either
 * add it to or remove it from the timers list.
 **/
static void
nih_dbus_timeout_toggled (DBusTimeout *timeout,
			  void        *data)
{
	NihTimer *timer;
	int       interval;

	nih_assert (timeout != NULL);

	timer = dbus_timeout_get_data (timeout);
	nih_assert (timer != NULL);

	if (dbus_timeout_get_enabled (timeout)) {
		nih_list_add (nih_timers, &timer->entry);
	} else {
		nih_list_remove (&timer->entry);
	}

	/* D-Bus may toggle the timer in an attempt to change the timeout */
	interval = dbus_timeout_get_interval (timeout);

	timer->period = (interval - 1) / 1000 + 1;
	timer->due = time (NULL) + timer->period;
}

/**
 * nih_dbus_timer:
 * @timeout: D-Bus timeout event occurred for,
 * @timer: timer that triggered the call.
 *
 * Called because @timer has elapsed and we need to pass that onto the
 * underlying @timeout.
 **/
static void
nih_dbus_timer (DBusTimeout *timeout,
		NihTimer    *timer)
{
	nih_assert (timeout != NULL);
	nih_assert (timer != NULL);

	dbus_timeout_handle (timeout);
}


/**
 * nih_dbus_wakeup_main:
 * @data: not used.
 *
 * Called by D-Bus to wakeup the main loop.
 **/
static void
nih_dbus_wakeup_main (void *data)
{
	nih_main_loop_interrupt ();
}

/**
 * nih_dbus_callback:
 * @conn: D-Bus connection,
 * @loop: loop callback structure.
 *
 * Called on each iteration of our main loop to dispatch any remaining items
 * of data from the given D-Bus connection @conn so that messages will be
 * handled automatically.
 **/
static void
nih_dbus_callback (DBusConnection  *conn,
		   NihMainLoopFunc *loop)
{
	nih_assert (conn != NULL);
	nih_assert (loop != NULL);

	while (dbus_connection_dispatch (conn) == DBUS_DISPATCH_DATA_REMAINS)
		;
}


/**
 * nih_dbus_connection_disconnected:
 * @conn: D-Bus connection,
 * @message: D-Bus message received,
 * @handler: Disconnection handler.
 *
 * Called as a filter function to determine whether @conn has been
 * disconnected, and if so, call the user disconnect @handler function.
 *
 * Once the handler has been called, the connection will be automatically
 * unreferenced.
 *
 * Returns: result of handling the message.
 **/
static DBusHandlerResult
nih_dbus_connection_disconnected (DBusConnection           *conn,
				  DBusMessage              *message,
				  NihDBusDisconnectHandler  handler)
{
	nih_assert (conn != NULL);
	nih_assert (message != NULL);

	if (! dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL,
				      "Disconnected"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (! dbus_message_has_path (message, DBUS_PATH_LOCAL))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	/* Ok, it's really the disconnected signal, call the handler. */
	if (handler)
		handler (conn);

	dbus_connection_unref (conn);

	/* Lie.  We want other filter functions for this to be called so
	 * we unreference for each copy we hold.
	 */
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * nih_dbus_new_connection:
 * @server: D-Bus server,
 * @conn: new D-Bus connection,
 * @data: not used.
 *
 * Called by D-Bus because a new connection @conn has been made to @server;
 * we call the connect handler if set, and if that returns TRUE (or not set),
 * we reference the connection so it is not dropped and set it up with
 * our main loop.
 **/
static void
nih_dbus_new_connection (DBusServer     *server,
			 DBusConnection *conn,
			 void           *data)
{
	NihDBusConnectHandler    connect_handler;
	NihDBusDisconnectHandler disconnect_handler;

	nih_assert (server != NULL);
	nih_assert (conn != NULL);

	/* Call the connect handler if set, if it returns FALSE, drop the
	 * connection.
	 */
	connect_handler = dbus_server_get_data (server, connect_handler_slot);
	if (connect_handler && (! connect_handler (server, conn)))
		return;

	/* We're keeping the connection, reference it and hook it up to the
	 * main loop.
	 */
	dbus_connection_ref (conn);
	disconnect_handler = dbus_server_get_data (server,
						   disconnect_handler_slot);
	NIH_ZERO (nih_dbus_setup (conn, disconnect_handler));
}


/**
 * nih_dbus_object_new:
 * @parent: parent block,
 * @conn: D-Bus connection to associate with,
 * @path: path of object,
 * @interfaces: interfaces list to attach,
 * @data: data pointer.
 *
 * Creates a new D-Bus object with the attached list of @interfaces which
 * specify the methods, signals and properties that object will export
 * and the C functions that will marshal them.
 *
 * @interfaces should be a NULL-terminated array of pointers to
 * NihDBusInterface structures.  Normally this is constructed using pointers
 * to structures defined by nih-dbus-tool which provides all the necessary
 * glue arrays and functions.
 *
 * The object structure is allocated using nih_alloc() and connected to
 * the given @conn, it can be unregistered by freeing it and it will be
 * automatically unregistered should @conn be disconnected.
 *
 * If @parent is not NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned block will be freed too.
 *
 * Returns: new NihDBusObject structure on success, or NULL if
 * insufficient memory.
 **/
NihDBusObject *
nih_dbus_object_new (const void              *parent,
		     DBusConnection          *conn,
		     const char              *path,
		     const NihDBusInterface **interfaces,
		     void                    *data)
{
	NihDBusObject *object;

	nih_assert (conn != NULL);
	nih_assert (path != NULL);
	nih_assert (interfaces != NULL);

	object = nih_new (parent, NihDBusObject);
	if (! object)
		return NULL;

	object->path = nih_strdup (object, path);
	if (! object->path) {
		nih_free (object);
		return NULL;
	}

	object->conn = conn;
	object->data = data;
	object->interfaces = interfaces;
	object->registered = FALSE;

	if (! dbus_connection_register_object_path (object->conn, object->path,
						    &nih_dbus_object_vtable,
						    object)) {
		nih_free (object);
		return NULL;
	}

	object->registered = TRUE;
	nih_alloc_set_destructor (object,
				  (NihDestructor)nih_dbus_object_destroy);

	return object;
}

/**
 * nih_dbus_object_destroy:
 * @object: D-Bus object being destroyed.
 *
 * Destructor function for an NihDBusObject structure, ensures that it
 * is unregistered from the attached D-Bus connection and path.
 *
 * Returns: always zero.
 **/
static int
nih_dbus_object_destroy (NihDBusObject *object)
{
	nih_assert (object != NULL);

	if (object->registered) {
		object->registered = FALSE;
		dbus_connection_unregister_object_path (object->conn,
							object->path);
	}

	return 0;
}

/**
 * nih_dbus_object_unregister:
 * @conn: D-Bus connection,
 * @object: D-Bus object to destroy.
 *
 * Called by D-Bus to unregister the @object attached to the D-Bus connection
 * @conn, requires us to free the attached structure.
 **/
static void
nih_dbus_object_unregister (DBusConnection *conn,
			    NihDBusObject  *object)
{
	nih_assert (conn != NULL);
	nih_assert (object != NULL);
	nih_assert (object->conn == conn);

	if (object->registered) {
		object->registered = FALSE;
		nih_free (object);
	}
}


/**
 * nih_dbus_object_message:
 * @conn: D-Bus connection,
 * @message: D-Bus message received,
 * @object: Object that received the message.
 *
 * Called by D-Bus when a @message is received for a registered @object.  We
 * handle messages related to introspection and properties ourselves,
 * otherwise the method invoked is located in the @object's interfaces array
 * and the marshaller function called to handle it.
 *
 * Returns: result of handling the message.
 **/
static DBusHandlerResult
nih_dbus_object_message (DBusConnection *conn,
			 DBusMessage    *message,
			 NihDBusObject  *object)
{
	const NihDBusInterface **interface;

	nih_assert (conn != NULL);
	nih_assert (message != NULL);
	nih_assert (object != NULL);
	nih_assert (object->conn == conn);

	/* Handle introspection internally */
	if (dbus_message_is_method_call (
		    message, DBUS_INTERFACE_INTROSPECTABLE, "Introspect"))
		return nih_dbus_object_introspect (conn, message, object);

	/* FIXME handle properties */
	if (dbus_message_is_method_call (
		    message, DBUS_INTERFACE_PROPERTIES, "Get"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_method_call (
		    message, DBUS_INTERFACE_PROPERTIES, "Set"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_method_call (
		    message, DBUS_INTERFACE_PROPERTIES, "GetAll"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;


	/* No built-in handling, locate a marshaller function in the defined
	 * interfaces that can handle it.
	 */
	for (interface = object->interfaces; interface && *interface;
	     interface++) {
		const NihDBusMethod *method;

		for (method = (*interface)->methods; method && method->name;
		     method++) {
			nih_assert (method->marshaller != NULL);

			if (dbus_message_is_method_call (message,
							 (*interface)->name,
							 method->name)) {
				NihDBusMessage    *msg;
				DBusHandlerResult  result;

				msg = nih_new (NULL, NihDBusMessage);
				if (! msg)
					return DBUS_HANDLER_RESULT_NEED_MEMORY;

				msg->conn = conn;
				msg->message = message;

				dbus_message_ref (msg->message);

				result = method->marshaller (object, msg);

				dbus_message_unref (msg->message);

				nih_free (msg);

				return result;
			}
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * nih_dbus_object_introspect:
 * @conn: D-Bus connection,
 * @message: D-Bus message received,
 * @object: Object that received the message.
 *
 * Called because the D-Bus introspection method has been invoked on @object,
 * we return an XML description of the object's interfaces, methods, signals
 * and properties based on its interfaces array.
 *
 * Returns: result of handling the message.
 **/
static DBusHandlerResult
nih_dbus_object_introspect (DBusConnection *conn,
			    DBusMessage    *message,
			    NihDBusObject  *object)
{
	const NihDBusInterface **interface;
	char                    *xml = NULL, **children = NULL, **child;
	DBusMessage             *reply = NULL;
	int                      have_props = FALSE;

	nih_assert (conn != NULL);
	nih_assert (message != NULL);
	nih_assert (object != NULL);
	nih_assert (object->conn == conn);

	xml = nih_strdup (NULL, DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE);
	if (! xml)
		goto error;

	/* Root node */
	if (! nih_strcat_sprintf (&xml, NULL, "<node name=\"%s\">\n",
				  object->path))
		goto error;

	/* Obviously we support introspection */
	if (! nih_strcat_sprintf (&xml, NULL,
				  "  <interface name=\"%s\">\n"
				  "    <method name=\"Introspect\">\n"
				  "      <arg name=\"data\" type=\"s\" direction=\"out\"/>\n"
				  "    </method>\n"
				  "  </interface>\n",
				  DBUS_INTERFACE_INTROSPECTABLE))
		goto error;

	/* Add each interface definition */
	for (interface = object->interfaces; interface && *interface;
	     interface++) {
		const NihDBusMethod   *method;
		const NihDBusSignal   *signal;
		const NihDBusProperty *property;

		if (! nih_strcat_sprintf (&xml, NULL,
					  "  <interface name=\"%s\">\n",
					  (*interface)->name))
			goto error;

		for (method = (*interface)->methods; method && method->name;
		     method++) {
			const NihDBusArg *arg;

			if (! nih_strcat_sprintf (&xml, NULL,
						  "    <method name=\"%s\">\n",
						  method->name))
				goto error;

			for (arg = method->args; arg && arg->type; arg++) {
				if (! nih_strcat_sprintf (
					    &xml, NULL,
					    "      <arg name=\"%s\" type=\"%s\""
					    " direction=\"%s\"/>\n",
					    arg->name, arg->type,
					    (arg->dir == NIH_DBUS_ARG_IN ? "in"
					     : "out")))
					goto error;
			}

			if (! nih_strcat (&xml, NULL, "    </method>\n"))
				goto error;
		}

		for (signal = (*interface)->signals; signal && signal->name;
		     signal++) {
			const NihDBusArg *arg;

			if (! nih_strcat_sprintf (&xml, NULL,
						  "    <signal name=\"%s\">\n",
						  signal->name))
				goto error;

			for (arg = signal->args; arg && arg->type; arg++) {
				if (! nih_strcat_sprintf (
					    &xml, NULL,
					    "      <arg name=\"%s\" type=\"%s\"/>\n",
					    arg->name, arg->type))
					goto error;
			}

			if (! nih_strcat (&xml, NULL, "    </signal>\n"))
				goto error;
		}

		for (property = (*interface)->properties;
		     property && property->name; property++) {
			have_props = TRUE;
			if (! nih_strcat_sprintf (
				    &xml, NULL,
				    "    <property name=\"%s\" type=\"%s\" "
				    "access=\"%s\"/>\n",
				    property->name, property->type,
				    (property->access == NIH_DBUS_READ ? "read"
				     : (property->access == NIH_DBUS_WRITE
					? "write" : "readwrite"))))
				goto error;
		}

		if (! nih_strcat (&xml, NULL, "  </interface>\n"))
		    goto error;
	}

	/* We may also support properties, but don't want to announce that
	 * unless we really do have some.
	 */
	if (have_props)
		if (! nih_strcat_sprintf (
			    &xml, NULL,
			    "  <interface name=\"%s\">\n"
			    "    <method name=\"Get\">\n"
			    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
			    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
			    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
			    "    </method>\n"
			    "    <method name=\"Set\">\n"
			    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
			    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
			    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
			    "    </method>\n"
			    "    <method name=\"GetAll\">\n"
			    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
			    "      <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>\n"
			    "    </method>\n"
			    "  </interface>\n",
			    DBUS_INTERFACE_PROPERTIES))
			goto error;

	/* Add node items for children */
	if (! dbus_connection_list_registered (conn, object->path, &children))
		goto error;

	for (child = children; *child; child++) {
		if (! nih_strcat_sprintf (&xml, NULL, "  <node name=\"%s\"/>\n",
					  *child))
			goto error;
	}

	if (! nih_strcat (&xml, NULL, "</node>\n"))
		goto error;

	dbus_free_string_array (children);
	children = NULL;


	/* Generate and send the reply */
	reply = dbus_message_new_method_return (message);
	if (! reply)
		goto error;

	if (! dbus_message_append_args (reply,
					DBUS_TYPE_STRING, &xml,
					DBUS_TYPE_INVALID))
		goto error;

	if (! dbus_connection_send (conn, reply, NULL))
		goto error;

	dbus_message_unref (reply);

	nih_free (xml);

	return DBUS_HANDLER_RESULT_HANDLED;

error:
	if (reply)
		dbus_message_unref (reply);
	if (children)
		dbus_free_string_array (children);
	if (xml)
		nih_free (xml);

	return DBUS_HANDLER_RESULT_NEED_MEMORY;
}


/**
 * nih_dbus_path:
 * @parent: parent block of allocation,
 * @root: root of path.
 *
 * Generates a D-Bus path suitable for object registration rooted at
 * @root with each of the further elements joined with "/" separators and
 * appended after non-permissible characters are removed.
 *
 * The final argument to this function must be NULL to signify the end
 * of elements.
 *
 * If @parent is not NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned block will be freed too.
 *
 * Returns: newly allocated string or NULL if insufficient memory.
 **/
char *
nih_dbus_path (const void *parent,
	       const char *root,
	       ...)
{
	const char *arg, *ptr;
	char       *path;
	va_list     args;
	size_t      len;

	nih_assert (root != NULL);

	/* First work out how much space we'll need */
	len = strlen (root);

	va_start (args, root);
	for (arg = va_arg (args, const char *); arg != NULL;
	     arg = va_arg (args, const char *)) {
		len += 1;

		for (ptr = arg; *ptr != '\0'; ptr++) {
			if (   ((*ptr >= 'a') && (*ptr <= 'z'))
			    || ((*ptr >= 'A') && (*ptr <= 'Z'))
			    || ((*ptr >= '0') && (*ptr <= '9'))) {
				len += 1;
			} else {
				len += 3;
			}
		}
	}
	va_end (args);

	/* Now we can allocate it */
	path = nih_alloc (parent, len + 1);
	if (! path)
		return NULL;

	/* And copy the elements in */
	strcpy (path, root);
	len = strlen (root);

	va_start (args, root);
	for (arg = va_arg (args, const char *); arg != NULL;
	     arg = va_arg (args, const char *)) {
		path[len++] = '/';

		for (ptr = arg; *ptr != '\0'; ptr++) {
			if (   ((*ptr >= 'a') && (*ptr <= 'z'))
			    || ((*ptr >= 'A') && (*ptr <= 'Z'))
			    || ((*ptr >= '0') && (*ptr <= '9'))) {
				path[len++] = *ptr;
			} else {
				path[len++] = '_';

				sprintf (path + len, "%02x", *ptr);
				len += 2;
			}
		}
	}
	va_end (args);

	path[len] = '\0';

	return path;
}
