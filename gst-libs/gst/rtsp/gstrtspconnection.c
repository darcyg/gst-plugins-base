/* GStreamer
 * Copyright (C) <2005-2009> Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/*
 * Unless otherwise indicated, Source Code is licensed under MIT license.
 * See further explanation attached in License Statement (distributed in the file
 * LICENSE).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * SECTION:gstrtspconnection
 * @title: GstRTSPConnection
 * @short_description: manage RTSP connections
 * @see_also: gstrtspurl
 *
 * This object manages the RTSP connection to the server. It provides function
 * to receive and send bytes and messages.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* we include this here to get the G_OS_* defines */
#include <glib.h>
#include <gst/gst.h>
#include <gst/base/base.h>

/* necessary for IP_TOS define */
#include <gio/gnetworking.h>

#include "gstrtspconnection.h"

#ifdef IP_TOS
union gst_sockaddr
{
  struct sockaddr sa;
  struct sockaddr_in sa_in;
  struct sockaddr_in6 sa_in6;
  struct sockaddr_storage sa_stor;
};
#endif

typedef struct
{
  gint state;
  guint save;
  guchar out[3];                /* the size must be evenly divisible by 3 */
  guint cout;
  guint coutl;
} DecodeCtx;

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

typedef enum
{
  TUNNEL_STATE_NONE,
  TUNNEL_STATE_GET,
  TUNNEL_STATE_POST,
  TUNNEL_STATE_COMPLETE
} GstRTSPTunnelState;

#define TUNNELID_LEN   24

struct _GstRTSPConnection
{
  /*< private > */
  /* URL for the remote connection */
  GstRTSPUrl *url;
  GstRTSPVersion version;

  gboolean server;
  GSocketClient *client;
  GIOStream *stream0;
  GIOStream *stream1;

  GInputStream *input_stream;
  GOutputStream *output_stream;
  /* this is a read source we add on the write socket in tunneled mode to be
   * able to detect when client disconnects the GET channel */
  GInputStream *control_stream;

  /* connection state */
  GSocket *read_socket;
  GSocket *write_socket;
  GSocket *socket0, *socket1;
  gboolean manual_http;
  gboolean may_cancel;
  GCancellable *cancellable;

  gchar tunnelid[TUNNELID_LEN];
  gboolean tunneled;
  GstRTSPTunnelState tstate;

  /* the remote and local ip */
  gchar *remote_ip;
  gchar *local_ip;

  gint read_ahead;

  gchar *initial_buffer;
  gsize initial_buffer_offset;

  gboolean remember_session_id; /* remember the session id or not */

  /* Session state */
  gint cseq;                    /* sequence number */
  gchar session_id[512];        /* session id */
  gint timeout;                 /* session timeout in seconds */
  GTimer *timer;                /* timeout timer */

  /* Authentication */
  GstRTSPAuthMethod auth_method;
  gchar *username;
  gchar *passwd;
  GHashTable *auth_params;

  /* TLS */
  GTlsDatabase *tls_database;
  GTlsInteraction *tls_interaction;

  GstRTSPConnectionAcceptCertificateFunc accept_certificate_func;
  GDestroyNotify accept_certificate_destroy_notify;
  gpointer accept_certificate_user_data;

  DecodeCtx ctx;
  DecodeCtx *ctxp;

  gchar *proxy_host;
  guint proxy_port;
};

enum
{
  STATE_START = 0,
  STATE_DATA_HEADER,
  STATE_DATA_BODY,
  STATE_READ_LINES,
  STATE_END,
  STATE_LAST
};

enum
{
  READ_AHEAD_EOH = -1,          /* end of headers */
  READ_AHEAD_CRLF = -2,
  READ_AHEAD_CRLFCR = -3
};

/* a structure for constructing RTSPMessages */
typedef struct
{
  gint state;
  GstRTSPResult status;
  guint8 buffer[4096];
  guint offset;

  guint line;
  guint8 *body_data;
  glong body_len;
} GstRTSPBuilder;

/* function prototypes */
static void add_auth_header (GstRTSPConnection * conn,
    GstRTSPMessage * message);

static void
build_reset (GstRTSPBuilder * builder)
{
  g_free (builder->body_data);
  memset (builder, 0, sizeof (GstRTSPBuilder));
}

static gboolean
tls_accept_certificate (GTlsConnection * conn, GTlsCertificate * peer_cert,
    GTlsCertificateFlags errors, GstRTSPConnection * rtspconn)
{
  GError *error = NULL;
  gboolean accept = FALSE;

  if (rtspconn->tls_database) {
    GSocketConnectable *peer_identity;
    GTlsCertificateFlags validation_flags;

    GST_DEBUG ("TLS peer certificate not accepted, checking user database...");

    peer_identity =
        g_tls_client_connection_get_server_identity (G_TLS_CLIENT_CONNECTION
        (conn));

    errors =
        g_tls_database_verify_chain (rtspconn->tls_database, peer_cert,
        G_TLS_DATABASE_PURPOSE_AUTHENTICATE_SERVER, peer_identity,
        g_tls_connection_get_interaction (conn), G_TLS_DATABASE_VERIFY_NONE,
        NULL, &error);

    if (error)
      goto verify_error;

    validation_flags = gst_rtsp_connection_get_tls_validation_flags (rtspconn);

    accept = ((errors & validation_flags) == 0);
    if (accept)
      GST_DEBUG ("Peer certificate accepted");
    else
      GST_DEBUG ("Peer certificate not accepted (errors: 0x%08X)", errors);
  }

  if (!accept && rtspconn->accept_certificate_func) {
    accept =
        rtspconn->accept_certificate_func (conn, peer_cert, errors,
        rtspconn->accept_certificate_user_data);
    GST_DEBUG ("Peer certificate %saccepted by accept-certificate function",
        accept ? "" : "not ");
  }

  return accept;

/* ERRORS */
verify_error:
  {
    GST_ERROR ("An error occurred while verifying the peer certificate: %s",
        error->message);
    g_clear_error (&error);
    return FALSE;
  }
}

static void
socket_client_event (GSocketClient * client, GSocketClientEvent event,
    GSocketConnectable * connectable, GTlsConnection * connection,
    GstRTSPConnection * rtspconn)
{
  if (event == G_SOCKET_CLIENT_TLS_HANDSHAKING) {
    GST_DEBUG ("TLS handshaking about to start...");

    g_signal_connect (connection, "accept-certificate",
        (GCallback) tls_accept_certificate, rtspconn);

    g_tls_connection_set_interaction (connection, rtspconn->tls_interaction);
  }
}

/**
 * gst_rtsp_connection_create:
 * @url: a #GstRTSPUrl
 * @conn: (out) (transfer full): storage for a #GstRTSPConnection
 *
 * Create a newly allocated #GstRTSPConnection from @url and store it in @conn.
 * The connection will not yet attempt to connect to @url, use
 * gst_rtsp_connection_connect().
 *
 * A copy of @url will be made.
 *
 * Returns: #GST_RTSP_OK when @conn contains a valid connection.
 */
GstRTSPResult
gst_rtsp_connection_create (const GstRTSPUrl * url, GstRTSPConnection ** conn)
{
  GstRTSPConnection *newconn;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (url != NULL, GST_RTSP_EINVAL);

  newconn = g_new0 (GstRTSPConnection, 1);

  newconn->may_cancel = TRUE;
  newconn->cancellable = g_cancellable_new ();
  newconn->client = g_socket_client_new ();

  if (url->transports & GST_RTSP_LOWER_TRANS_TLS)
    g_socket_client_set_tls (newconn->client, TRUE);

  g_signal_connect (newconn->client, "event", (GCallback) socket_client_event,
      newconn);

  newconn->url = gst_rtsp_url_copy (url);
  newconn->timer = g_timer_new ();
  newconn->timeout = 60;
  newconn->cseq = 1;

  newconn->remember_session_id = TRUE;

  newconn->auth_method = GST_RTSP_AUTH_NONE;
  newconn->username = NULL;
  newconn->passwd = NULL;
  newconn->auth_params = NULL;
  newconn->version = 0;

  *conn = newconn;

  return GST_RTSP_OK;
}

static gboolean
collect_addresses (GSocket * socket, gchar ** ip, guint16 * port,
    gboolean remote, GError ** error)
{
  GSocketAddress *addr;

  if (remote)
    addr = g_socket_get_remote_address (socket, error);
  else
    addr = g_socket_get_local_address (socket, error);
  if (!addr)
    return FALSE;

  if (ip)
    *ip = g_inet_address_to_string (g_inet_socket_address_get_address
        (G_INET_SOCKET_ADDRESS (addr)));
  if (port)
    *port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (addr));

  g_object_unref (addr);

  return TRUE;
}


/**
 * gst_rtsp_connection_create_from_socket:
 * @socket: a #GSocket
 * @ip: the IP address of the other end
 * @port: the port used by the other end
 * @initial_buffer: data already read from @fd
 * @conn: (out) (transfer full): storage for a #GstRTSPConnection
 *
 * Create a new #GstRTSPConnection for handling communication on the existing
 * socket @socket. The @initial_buffer contains zero terminated data already
 * read from @socket which should be used before starting to read new data.
 *
 * Returns: #GST_RTSP_OK when @conn contains a valid connection.
 */
/* FIXME 2.0 We don't need the ip and port since they can be got from the
 * GSocket */
GstRTSPResult
gst_rtsp_connection_create_from_socket (GSocket * socket, const gchar * ip,
    guint16 port, const gchar * initial_buffer, GstRTSPConnection ** conn)
{
  GstRTSPConnection *newconn = NULL;
  GstRTSPUrl *url;
  GstRTSPResult res;
  GError *err = NULL;
  gchar *local_ip;
  GIOStream *stream;

  g_return_val_if_fail (G_IS_SOCKET (socket), GST_RTSP_EINVAL);
  g_return_val_if_fail (ip != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  if (!collect_addresses (socket, &local_ip, NULL, FALSE, &err))
    goto getnameinfo_failed;

  /* create a url for the client address */
  url = g_new0 (GstRTSPUrl, 1);
  url->host = g_strdup (ip);
  url->port = port;

  /* now create the connection object */
  GST_RTSP_CHECK (gst_rtsp_connection_create (url, &newconn), newconn_failed);
  gst_rtsp_url_free (url);

  stream = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  /* both read and write initially */
  newconn->server = TRUE;
  newconn->socket0 = socket;
  newconn->stream0 = stream;
  newconn->write_socket = newconn->read_socket = newconn->socket0;
  newconn->input_stream = g_io_stream_get_input_stream (stream);
  newconn->output_stream = g_io_stream_get_output_stream (stream);
  newconn->control_stream = NULL;
  newconn->remote_ip = g_strdup (ip);
  newconn->local_ip = local_ip;
  newconn->initial_buffer = g_strdup (initial_buffer);

  *conn = newconn;

  return GST_RTSP_OK;

  /* ERRORS */
getnameinfo_failed:
  {
    GST_ERROR ("failed to get local address: %s", err->message);
    g_clear_error (&err);
    return GST_RTSP_ERROR;
  }
newconn_failed:
  {
    GST_ERROR ("failed to make connection");
    g_free (local_ip);
    gst_rtsp_url_free (url);
    return res;
  }
}

/**
 * gst_rtsp_connection_accept:
 * @socket: a socket
 * @conn: (out) (transfer full): storage for a #GstRTSPConnection
 * @cancellable: a #GCancellable to cancel the operation
 *
 * Accept a new connection on @socket and create a new #GstRTSPConnection for
 * handling communication on new socket.
 *
 * Returns: #GST_RTSP_OK when @conn contains a valid connection.
 */
GstRTSPResult
gst_rtsp_connection_accept (GSocket * socket, GstRTSPConnection ** conn,
    GCancellable * cancellable)
{
  GError *err = NULL;
  gchar *ip;
  guint16 port;
  GSocket *client_sock;
  GstRTSPResult ret;

  g_return_val_if_fail (G_IS_SOCKET (socket), GST_RTSP_EINVAL);
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  client_sock = g_socket_accept (socket, cancellable, &err);
  if (!client_sock)
    goto accept_failed;

  /* get the remote ip address and port */
  if (!collect_addresses (client_sock, &ip, &port, TRUE, &err))
    goto getnameinfo_failed;

  ret =
      gst_rtsp_connection_create_from_socket (client_sock, ip, port, NULL,
      conn);
  g_object_unref (client_sock);
  g_free (ip);

  return ret;

  /* ERRORS */
accept_failed:
  {
    GST_DEBUG ("Accepting client failed: %s", err->message);
    g_clear_error (&err);
    return GST_RTSP_ESYS;
  }
getnameinfo_failed:
  {
    GST_DEBUG ("getnameinfo failed: %s", err->message);
    g_clear_error (&err);
    if (!g_socket_close (client_sock, &err)) {
      GST_DEBUG ("Closing socket failed: %s", err->message);
      g_clear_error (&err);
    }
    g_object_unref (client_sock);
    return GST_RTSP_ERROR;
  }
}

/**
 * gst_rtsp_connection_get_tls:
 * @conn: a #GstRTSPConnection
 * @error: #GError for error reporting, or NULL to ignore.
 *
 * Get the TLS connection of @conn.
 *
 * For client side this will return the #GTlsClientConnection when connected
 * over TLS.
 *
 * For server side connections, this function will create a GTlsServerConnection
 * when called the first time and will return that same connection on subsequent
 * calls. The server is then responsible for configuring the TLS connection.
 *
 * Returns: (transfer none): the TLS connection for @conn.
 *
 * Since: 1.2
 */
GTlsConnection *
gst_rtsp_connection_get_tls (GstRTSPConnection * conn, GError ** error)
{
  GTlsConnection *result;

  if (G_IS_TLS_CONNECTION (conn->stream0)) {
    /* we already had one, return it */
    result = G_TLS_CONNECTION (conn->stream0);
  } else if (conn->server) {
    /* no TLS connection but we are server, make one */
    result = (GTlsConnection *)
        g_tls_server_connection_new (conn->stream0, NULL, error);
    if (result) {
      g_object_unref (conn->stream0);
      conn->stream0 = G_IO_STREAM (result);
      conn->input_stream = g_io_stream_get_input_stream (conn->stream0);
      conn->output_stream = g_io_stream_get_output_stream (conn->stream0);
    }
  } else {
    /* client */
    result = NULL;
    g_set_error (error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
        "client not connected with TLS");
  }
  return result;
}

/**
 * gst_rtsp_connection_set_tls_validation_flags:
 * @conn: a #GstRTSPConnection
 * @flags: the validation flags.
 *
 * Sets the TLS validation flags to be used to verify the peer
 * certificate when a TLS connection is established.
 *
 * Returns: TRUE if the validation flags are set correctly, or FALSE if
 * @conn is NULL or is not a TLS connection.
 *
 * Since: 1.2.1
 */
gboolean
gst_rtsp_connection_set_tls_validation_flags (GstRTSPConnection * conn,
    GTlsCertificateFlags flags)
{
  gboolean res = FALSE;

  g_return_val_if_fail (conn != NULL, FALSE);

  res = g_socket_client_get_tls (conn->client);
  if (res)
    g_socket_client_set_tls_validation_flags (conn->client, flags);

  return res;
}

/**
 * gst_rtsp_connection_get_tls_validation_flags:
 * @conn: a #GstRTSPConnection
 *
 * Gets the TLS validation flags used to verify the peer certificate
 * when a TLS connection is established.
 *
 * Returns: the validationg flags.
 *
 * Since: 1.2.1
 */
GTlsCertificateFlags
gst_rtsp_connection_get_tls_validation_flags (GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, 0);

  return g_socket_client_get_tls_validation_flags (conn->client);
}

/**
 * gst_rtsp_connection_set_tls_database:
 * @conn: a #GstRTSPConnection
 * @database: a #GTlsDatabase
 *
 * Sets the anchor certificate authorities database. This certificate
 * database will be used to verify the server's certificate in case it
 * can't be verified with the default certificate database first.
 *
 * Since: 1.4
 */
void
gst_rtsp_connection_set_tls_database (GstRTSPConnection * conn,
    GTlsDatabase * database)
{
  GTlsDatabase *old_db;

  g_return_if_fail (conn != NULL);

  if (database)
    g_object_ref (database);

  old_db = conn->tls_database;
  conn->tls_database = database;

  if (old_db)
    g_object_unref (old_db);
}

/**
 * gst_rtsp_connection_get_tls_database:
 * @conn: a #GstRTSPConnection
 *
 * Gets the anchor certificate authorities database that will be used
 * after a server certificate can't be verified with the default
 * certificate database.
 *
 * Returns: (transfer full): the anchor certificate authorities database, or NULL if no
 * database has been previously set. Use g_object_unref() to release the
 * certificate database.
 *
 * Since: 1.4
 */
GTlsDatabase *
gst_rtsp_connection_get_tls_database (GstRTSPConnection * conn)
{
  GTlsDatabase *result;

  g_return_val_if_fail (conn != NULL, NULL);

  if ((result = conn->tls_database))
    g_object_ref (result);

  return result;
}

/**
 * gst_rtsp_connection_set_tls_interaction:
 * @conn: a #GstRTSPConnection
 * @interaction: a #GTlsInteraction
 *
 * Sets a #GTlsInteraction object to be used when the connection or certificate
 * database need to interact with the user. This will be used to prompt the
 * user for passwords where necessary.
 *
 * Since: 1.6
 */
void
gst_rtsp_connection_set_tls_interaction (GstRTSPConnection * conn,
    GTlsInteraction * interaction)
{
  GTlsInteraction *old_interaction;

  g_return_if_fail (conn != NULL);

  if (interaction)
    g_object_ref (interaction);

  old_interaction = conn->tls_interaction;
  conn->tls_interaction = interaction;

  if (old_interaction)
    g_object_unref (old_interaction);
}

/**
 * gst_rtsp_connection_get_tls_interaction:
 * @conn: a #GstRTSPConnection
 *
 * Gets a #GTlsInteraction object to be used when the connection or certificate
 * database need to interact with the user. This will be used to prompt the
 * user for passwords where necessary.
 *
 * Returns: (transfer full): a reference on the #GTlsInteraction. Use
 * g_object_unref() to release.
 *
 * Since: 1.6
 */
GTlsInteraction *
gst_rtsp_connection_get_tls_interaction (GstRTSPConnection * conn)
{
  GTlsInteraction *result;

  g_return_val_if_fail (conn != NULL, NULL);

  if ((result = conn->tls_interaction))
    g_object_ref (result);

  return result;
}

/**
 * gst_rtsp_connection_set_accept_certificate_func:
 * @conn: a #GstRTSPConnection
 * @func: a #GstRTSPConnectionAcceptCertificateFunc to check certificates
 * @destroy_notify: #GDestroyNotify for @user_data
 * @user_data: User data passed to @func
 *
 * Sets a custom accept-certificate function for checking certificates for
 * validity. This will directly map to #GTlsConnection 's "accept-certificate"
 * signal and be performed after the default checks of #GstRTSPConnection
 * (checking against the #GTlsDatabase with the given #GTlsCertificateFlags)
 * have failed. If no #GTlsDatabase is set on this connection, only @func will
 * be called.
 *
 * Since: 1.14
 */
void
gst_rtsp_connection_set_accept_certificate_func (GstRTSPConnection * conn,
    GstRTSPConnectionAcceptCertificateFunc func,
    gpointer user_data, GDestroyNotify destroy_notify)
{
  if (conn->accept_certificate_destroy_notify)
    conn->
        accept_certificate_destroy_notify (conn->accept_certificate_user_data);
  conn->accept_certificate_func = func;
  conn->accept_certificate_user_data = user_data;
  conn->accept_certificate_destroy_notify = destroy_notify;
}

static GstRTSPResult
setup_tunneling (GstRTSPConnection * conn, GTimeVal * timeout, gchar * uri,
    GstRTSPMessage * response)
{
  gint i;
  GstRTSPResult res;
  gchar *value;
  guint16 url_port;
  GstRTSPMessage *msg;
  gboolean old_http;
  GstRTSPUrl *url;
  GError *error = NULL;
  GSocketConnection *connection;
  GSocket *socket;
  gchar *connection_uri = NULL;
  gchar *request_uri = NULL;
  gchar *host = NULL;

  url = conn->url;

  gst_rtsp_url_get_port (url, &url_port);
  host = g_strdup_printf ("%s:%d", url->host, url_port);

  /* create a random sessionid */
  for (i = 0; i < TUNNELID_LEN; i++)
    conn->tunnelid[i] = g_random_int_range ('a', 'z');
  conn->tunnelid[TUNNELID_LEN - 1] = '\0';

  /* create the GET request for the read connection */
  GST_RTSP_CHECK (gst_rtsp_message_new_request (&msg, GST_RTSP_GET, uri),
      no_message);
  msg->type = GST_RTSP_MESSAGE_HTTP_REQUEST;

  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_X_SESSIONCOOKIE,
      conn->tunnelid);
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_ACCEPT,
      "application/x-rtsp-tunnelled");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CACHE_CONTROL, "no-cache");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_PRAGMA, "no-cache");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_HOST, host);
  add_auth_header (conn, msg);

  /* we need to temporarily set conn->tunneled to FALSE to prevent the HTTP
   * request from being base64 encoded */
  conn->tunneled = FALSE;
  GST_RTSP_CHECK (gst_rtsp_connection_send (conn, msg, timeout), write_failed);
  gst_rtsp_message_free (msg);
  conn->tunneled = TRUE;

  /* receive the response to the GET request */
  /* we need to temporarily set manual_http to TRUE since
   * gst_rtsp_connection_receive() will treat the HTTP response as a parsing
   * failure otherwise */
  old_http = conn->manual_http;
  conn->manual_http = TRUE;
  GST_RTSP_CHECK (gst_rtsp_connection_receive (conn, response, timeout),
      read_failed);
  conn->manual_http = old_http;

  if (response->type != GST_RTSP_MESSAGE_HTTP_RESPONSE ||
      response->type_data.response.code != GST_RTSP_STS_OK)
    goto wrong_result;

  if (gst_rtsp_message_get_header (response, GST_RTSP_HDR_X_SERVER_IP_ADDRESS,
          &value, 0) == GST_RTSP_OK) {
    g_free (url->host);
    url->host = g_strdup (value);
    g_free (conn->remote_ip);
    conn->remote_ip = g_strdup (value);
  }

  connection_uri = g_strdup_printf ("http://%s:%d%s%s%s", url->host, url_port,
      url->abspath, url->query ? "?" : "", url->query ? url->query : "");

  /* connect to the host/port */
  if (conn->proxy_host) {
    connection = g_socket_client_connect_to_host (conn->client,
        conn->proxy_host, conn->proxy_port, conn->cancellable, &error);
    request_uri = g_strdup (connection_uri);
  } else {
    connection = g_socket_client_connect_to_uri (conn->client,
        connection_uri, 0, conn->cancellable, &error);
    request_uri =
        g_strdup_printf ("%s%s%s", url->abspath,
        url->query ? "?" : "", url->query ? url->query : "");
  }
  if (connection == NULL)
    goto connect_failed;

  socket = g_socket_connection_get_socket (connection);

  /* get remote address */
  g_free (conn->remote_ip);
  conn->remote_ip = NULL;

  if (!collect_addresses (socket, &conn->remote_ip, NULL, TRUE, &error))
    goto remote_address_failed;

  /* this is now our writing socket */
  conn->stream1 = G_IO_STREAM (connection);
  conn->socket1 = socket;
  conn->write_socket = conn->socket1;
  conn->output_stream = g_io_stream_get_output_stream (conn->stream1);
  conn->control_stream = NULL;

  /* create the POST request for the write connection */
  GST_RTSP_CHECK (gst_rtsp_message_new_request (&msg, GST_RTSP_POST,
          request_uri), no_message);
  msg->type = GST_RTSP_MESSAGE_HTTP_REQUEST;

  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_X_SESSIONCOOKIE,
      conn->tunnelid);
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_ACCEPT,
      "application/x-rtsp-tunnelled");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CONTENT_TYPE,
      "application/x-rtsp-tunnelled");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CACHE_CONTROL, "no-cache");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_PRAGMA, "no-cache");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_EXPIRES,
      "Sun, 9 Jan 1972 00:00:00 GMT");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CONTENT_LENGTH, "32767");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_HOST, host);
  add_auth_header (conn, msg);

  /* we need to temporarily set conn->tunneled to FALSE to prevent the HTTP
   * request from being base64 encoded */
  conn->tunneled = FALSE;
  GST_RTSP_CHECK (gst_rtsp_connection_send (conn, msg, timeout), write_failed);
  gst_rtsp_message_free (msg);
  conn->tunneled = TRUE;

exit:
  g_free (connection_uri);
  g_free (request_uri);
  g_free (host);

  return res;

  /* ERRORS */
no_message:
  {
    GST_ERROR ("failed to create request (%d)", res);
    goto exit;
  }
write_failed:
  {
    GST_ERROR ("write failed (%d)", res);
    gst_rtsp_message_free (msg);
    conn->tunneled = TRUE;
    goto exit;
  }
read_failed:
  {
    GST_ERROR ("read failed (%d)", res);
    conn->manual_http = FALSE;
    goto exit;
  }
wrong_result:
  {
    GST_ERROR ("got failure response %d %s",
        response->type_data.response.code, response->type_data.response.reason);
    res = GST_RTSP_ERROR;
    goto exit;
  }
connect_failed:
  {
    GST_ERROR ("failed to connect: %s", error->message);
    res = GST_RTSP_ERROR;
    g_clear_error (&error);
    goto exit;
  }
remote_address_failed:
  {
    GST_ERROR ("failed to resolve address: %s", error->message);
    g_object_unref (connection);
    g_clear_error (&error);
    return GST_RTSP_ERROR;
  }
}

/**
 * gst_rtsp_connection_connect_with_response:
 * @conn: a #GstRTSPConnection
 * @timeout: a #GTimeVal timeout
 * @response: a #GstRTSPMessage
 *
 * Attempt to connect to the url of @conn made with
 * gst_rtsp_connection_create(). If @timeout is %NULL this function can block
 * forever. If @timeout contains a valid timeout, this function will return
 * #GST_RTSP_ETIMEOUT after the timeout expired.  If @conn is set to tunneled,
 * @response will contain a response to the tunneling request messages.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK when a connection could be made.
 *
 * Since 1.8
 */
GstRTSPResult
gst_rtsp_connection_connect_with_response (GstRTSPConnection * conn,
    GTimeVal * timeout, GstRTSPMessage * response)
{
  GstRTSPResult res;
  GSocketConnection *connection;
  GSocket *socket;
  GError *error = NULL;
  gchar *connection_uri, *request_uri, *remote_ip;
  GstClockTime to;
  guint16 url_port;
  GstRTSPUrl *url;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->url != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->stream0 == NULL, GST_RTSP_EINVAL);

  to = timeout ? GST_TIMEVAL_TO_TIME (*timeout) : 0;
  g_socket_client_set_timeout (conn->client,
      (to + GST_SECOND - 1) / GST_SECOND);

  url = conn->url;

  gst_rtsp_url_get_port (url, &url_port);

  if (conn->tunneled) {
    connection_uri = g_strdup_printf ("http://%s:%d%s%s%s", url->host, url_port,
        url->abspath, url->query ? "?" : "", url->query ? url->query : "");
  } else {
    connection_uri = gst_rtsp_url_get_request_uri (url);
  }

  if (conn->proxy_host) {
    connection = g_socket_client_connect_to_host (conn->client,
        conn->proxy_host, conn->proxy_port, conn->cancellable, &error);
    request_uri = g_strdup (connection_uri);
  } else {
    connection = g_socket_client_connect_to_uri (conn->client,
        connection_uri, url_port, conn->cancellable, &error);

    /* use the relative component of the uri for non-proxy connections */
    request_uri = g_strdup_printf ("%s%s%s", url->abspath,
        url->query ? "?" : "", url->query ? url->query : "");
  }
  if (connection == NULL)
    goto connect_failed;

  /* get remote address */
  socket = g_socket_connection_get_socket (connection);

  if (!collect_addresses (socket, &remote_ip, NULL, TRUE, &error))
    goto remote_address_failed;

  g_free (conn->remote_ip);
  conn->remote_ip = remote_ip;
  conn->stream0 = G_IO_STREAM (connection);
  conn->socket0 = socket;
  /* this is our read socket */
  conn->read_socket = conn->socket0;
  conn->write_socket = conn->socket0;
  conn->input_stream = g_io_stream_get_input_stream (conn->stream0);
  conn->output_stream = g_io_stream_get_output_stream (conn->stream0);
  conn->control_stream = NULL;

  if (conn->tunneled) {
    res = setup_tunneling (conn, timeout, request_uri, response);
    if (res != GST_RTSP_OK)
      goto tunneling_failed;
  }
  g_free (connection_uri);
  g_free (request_uri);

  return GST_RTSP_OK;

  /* ERRORS */
connect_failed:
  {
    GST_ERROR ("failed to connect: %s", error->message);
    g_clear_error (&error);
    g_free (connection_uri);
    g_free (request_uri);
    return GST_RTSP_ERROR;
  }
remote_address_failed:
  {
    GST_ERROR ("failed to connect: %s", error->message);
    g_object_unref (connection);
    g_clear_error (&error);
    g_free (connection_uri);
    g_free (request_uri);
    return GST_RTSP_ERROR;
  }
tunneling_failed:
  {
    GST_ERROR ("failed to setup tunneling");
    g_free (connection_uri);
    g_free (request_uri);
    return res;
  }
}

static void
add_auth_header (GstRTSPConnection * conn, GstRTSPMessage * message)
{
  switch (conn->auth_method) {
    case GST_RTSP_AUTH_BASIC:{
      gchar *user_pass;
      gchar *user_pass64;
      gchar *auth_string;

      if (conn->username == NULL || conn->passwd == NULL)
        break;

      user_pass = g_strdup_printf ("%s:%s", conn->username, conn->passwd);
      user_pass64 = g_base64_encode ((guchar *) user_pass, strlen (user_pass));
      auth_string = g_strdup_printf ("Basic %s", user_pass64);

      gst_rtsp_message_take_header (message, GST_RTSP_HDR_AUTHORIZATION,
          auth_string);

      g_free (user_pass);
      g_free (user_pass64);
      break;
    }
    case GST_RTSP_AUTH_DIGEST:{
      gchar *response;
      gchar *auth_string, *auth_string2;
      gchar *realm;
      gchar *nonce;
      gchar *opaque;
      const gchar *uri;
      const gchar *method;

      /* we need to have some params set */
      if (conn->auth_params == NULL || conn->username == NULL ||
          conn->passwd == NULL)
        break;

      /* we need the realm and nonce */
      realm = (gchar *) g_hash_table_lookup (conn->auth_params, "realm");
      nonce = (gchar *) g_hash_table_lookup (conn->auth_params, "nonce");
      if (realm == NULL || nonce == NULL)
        break;

      method = gst_rtsp_method_as_text (message->type_data.request.method);
      uri = message->type_data.request.uri;

      response =
          gst_rtsp_generate_digest_auth_response (NULL, method, realm,
          conn->username, conn->passwd, uri, nonce);
      auth_string =
          g_strdup_printf ("Digest username=\"%s\", "
          "realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
          conn->username, realm, nonce, uri, response);
      g_free (response);

      opaque = (gchar *) g_hash_table_lookup (conn->auth_params, "opaque");
      if (opaque) {
        auth_string2 = g_strdup_printf ("%s, opaque=\"%s\"", auth_string,
            opaque);
        g_free (auth_string);
        auth_string = auth_string2;
      }
      gst_rtsp_message_take_header (message, GST_RTSP_HDR_AUTHORIZATION,
          auth_string);
      break;
    }
    default:
      /* Nothing to do */
      break;
  }
}

/**
 * gst_rtsp_connection_connect:
 * @conn: a #GstRTSPConnection
 * @timeout: a #GTimeVal timeout
 *
 * Attempt to connect to the url of @conn made with
 * gst_rtsp_connection_create(). If @timeout is %NULL this function can block
 * forever. If @timeout contains a valid timeout, this function will return
 * #GST_RTSP_ETIMEOUT after the timeout expired.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK when a connection could be made.
 */
GstRTSPResult
gst_rtsp_connection_connect (GstRTSPConnection * conn, GTimeVal * timeout)
{
  GstRTSPResult result;
  GstRTSPMessage response;

  memset (&response, 0, sizeof (response));
  gst_rtsp_message_init (&response);

  result = gst_rtsp_connection_connect_with_response (conn, timeout, &response);

  gst_rtsp_message_unset (&response);

  return result;
}

static void
gen_date_string (gchar * date_string, guint len)
{
  static const char wkdays[7][4] =
      { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char months[12][4] =
      { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
    "Nov", "Dec"
  };
  struct tm tm;
  time_t t;

  time (&t);

#ifdef HAVE_GMTIME_R
  gmtime_r (&t, &tm);
#else
  tm = *gmtime (&t);
#endif

  g_snprintf (date_string, len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
      wkdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
      tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static GstRTSPResult
write_bytes (GOutputStream * stream, const guint8 * buffer, guint * idx,
    guint size, gboolean block, GCancellable * cancellable)
{
  guint left;
  gssize r;
  GError *err = NULL;

  if (G_UNLIKELY (*idx > size))
    return GST_RTSP_ERROR;

  left = size - *idx;

  while (left) {
    if (block)
      r = g_output_stream_write (stream, (gchar *) & buffer[*idx], left,
          cancellable, &err);
    else
      r = g_pollable_output_stream_write_nonblocking (G_POLLABLE_OUTPUT_STREAM
          (stream), (gchar *) & buffer[*idx], left, cancellable, &err);
    if (G_UNLIKELY (r < 0))
      goto error;

    left -= r;
    *idx += r;
  }
  return GST_RTSP_OK;

  /* ERRORS */
error:
  {
    if (G_UNLIKELY (r == 0))
      return GST_RTSP_EEOF;

    GST_DEBUG ("%s", err->message);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error (&err);
      return GST_RTSP_EINTR;
    } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
      g_clear_error (&err);
      return GST_RTSP_EINTR;
    } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
      g_clear_error (&err);
      return GST_RTSP_ETIMEOUT;
    }
    g_clear_error (&err);
    return GST_RTSP_ESYS;
  }
}

static gint
fill_raw_bytes (GstRTSPConnection * conn, guint8 * buffer, guint size,
    gboolean block, GError ** err)
{
  gint out = 0;

  if (G_UNLIKELY (conn->initial_buffer != NULL)) {
    gsize left = strlen (&conn->initial_buffer[conn->initial_buffer_offset]);

    out = MIN (left, size);
    memcpy (buffer, &conn->initial_buffer[conn->initial_buffer_offset], out);

    if (left == (gsize) out) {
      g_free (conn->initial_buffer);
      conn->initial_buffer = NULL;
      conn->initial_buffer_offset = 0;
    } else
      conn->initial_buffer_offset += out;
  }

  if (G_LIKELY (size > (guint) out)) {
    gssize r;
    gsize count = size - out;
    if (block)
      r = g_input_stream_read (conn->input_stream, (gchar *) & buffer[out],
          count, conn->may_cancel ? conn->cancellable : NULL, err);
    else
      r = g_pollable_input_stream_read_nonblocking (G_POLLABLE_INPUT_STREAM
          (conn->input_stream), (gchar *) & buffer[out], count,
          conn->may_cancel ? conn->cancellable : NULL, err);

    if (G_UNLIKELY (r < 0)) {
      if (out == 0) {
        /* propagate the error */
        out = r;
      } else {
        /* we have some data ignore error */
        g_clear_error (err);
      }
    } else
      out += r;
  }

  return out;
}

static gint
fill_bytes (GstRTSPConnection * conn, guint8 * buffer, guint size,
    gboolean block, GError ** err)
{
  DecodeCtx *ctx = conn->ctxp;
  gint out = 0;

  if (ctx) {
    while (size > 0) {
      guint8 in[sizeof (ctx->out) * 4 / 3];
      gint r;

      while (size > 0 && ctx->cout < ctx->coutl) {
        /* we have some leftover bytes */
        *buffer++ = ctx->out[ctx->cout++];
        size--;
        out++;
      }

      /* got what we needed? */
      if (size == 0)
        break;

      /* try to read more bytes */
      r = fill_raw_bytes (conn, in, sizeof (in), block, err);
      if (r <= 0) {
        if (out == 0)
          out = r;
        break;
      }

      ctx->cout = 0;
      ctx->coutl =
          g_base64_decode_step ((gchar *) in, r, ctx->out, &ctx->state,
          &ctx->save);
    }
  } else {
    out = fill_raw_bytes (conn, buffer, size, block, err);
  }

  return out;
}

static GstRTSPResult
read_bytes (GstRTSPConnection * conn, guint8 * buffer, guint * idx, guint size,
    gboolean block)
{
  guint left;
  gint r;
  GError *err = NULL;

  if (G_UNLIKELY (*idx > size))
    return GST_RTSP_ERROR;

  left = size - *idx;

  while (left) {
    r = fill_bytes (conn, &buffer[*idx], left, block, &err);
    if (G_UNLIKELY (r <= 0))
      goto error;

    left -= r;
    *idx += r;
  }
  return GST_RTSP_OK;

  /* ERRORS */
error:
  {
    if (G_UNLIKELY (r == 0))
      return GST_RTSP_EEOF;

    GST_DEBUG ("%s", err->message);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error (&err);
      return GST_RTSP_EINTR;
    } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
      g_clear_error (&err);
      return GST_RTSP_EINTR;
    } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
      g_clear_error (&err);
      return GST_RTSP_ETIMEOUT;
    }
    g_clear_error (&err);
    return GST_RTSP_ESYS;
  }
}

/* The code below tries to handle clients using \r, \n or \r\n to indicate the
 * end of a line. It even does its best to handle clients which mix them (even
 * though this is a really stupid idea (tm).) It also handles Line White Space
 * (LWS), where a line end followed by whitespace is considered LWS. This is
 * the method used in RTSP (and HTTP) to break long lines.
 */
static GstRTSPResult
read_line (GstRTSPConnection * conn, guint8 * buffer, guint * idx, guint size,
    gboolean block)
{
  GstRTSPResult res;

  while (TRUE) {
    guint8 c;
    guint i;

    if (conn->read_ahead == READ_AHEAD_EOH) {
      /* the last call to read_line() already determined that we have reached
       * the end of the headers, so convey that information now */
      conn->read_ahead = 0;
      break;
    } else if (conn->read_ahead == READ_AHEAD_CRLF) {
      /* the last call to read_line() left off after having read \r\n */
      c = '\n';
    } else if (conn->read_ahead == READ_AHEAD_CRLFCR) {
      /* the last call to read_line() left off after having read \r\n\r */
      c = '\r';
    } else if (conn->read_ahead != 0) {
      /* the last call to read_line() left us with a character to start with */
      c = (guint8) conn->read_ahead;
      conn->read_ahead = 0;
    } else {
      /* read the next character */
      i = 0;
      res = read_bytes (conn, &c, &i, 1, block);
      if (G_UNLIKELY (res != GST_RTSP_OK))
        return res;
    }

    /* special treatment of line endings */
    if (c == '\r' || c == '\n') {
      guint8 read_ahead;

    retry:
      /* need to read ahead one more character to know what to do... */
      i = 0;
      res = read_bytes (conn, &read_ahead, &i, 1, block);
      if (G_UNLIKELY (res != GST_RTSP_OK))
        return res;

      if (read_ahead == ' ' || read_ahead == '\t') {
        if (conn->read_ahead == READ_AHEAD_CRLFCR) {
          /* got \r\n\r followed by whitespace, treat it as a normal line
           * followed by one starting with LWS */
          conn->read_ahead = read_ahead;
          break;
        } else {
          /* got LWS, change the line ending to a space and continue */
          c = ' ';
          conn->read_ahead = read_ahead;
        }
      } else if (conn->read_ahead == READ_AHEAD_CRLFCR) {
        if (read_ahead == '\r' || read_ahead == '\n') {
          /* got \r\n\r\r or \r\n\r\n, treat it as the end of the headers */
          conn->read_ahead = READ_AHEAD_EOH;
          break;
        } else {
          /* got \r\n\r followed by something else, this is not really
           * supported since we have probably just eaten the first character
           * of the body or the next message, so just ignore the second \r
           * and live with it... */
          conn->read_ahead = read_ahead;
          break;
        }
      } else if (conn->read_ahead == READ_AHEAD_CRLF) {
        if (read_ahead == '\r') {
          /* got \r\n\r so far, need one more character... */
          conn->read_ahead = READ_AHEAD_CRLFCR;
          goto retry;
        } else if (read_ahead == '\n') {
          /* got \r\n\n, treat it as the end of the headers */
          conn->read_ahead = READ_AHEAD_EOH;
          break;
        } else {
          /* found the end of a line, keep read_ahead for the next line */
          conn->read_ahead = read_ahead;
          break;
        }
      } else if (c == read_ahead) {
        /* got double \r or \n, treat it as the end of the headers */
        conn->read_ahead = READ_AHEAD_EOH;
        break;
      } else if (c == '\r' && read_ahead == '\n') {
        /* got \r\n so far, still need more to know what to do... */
        conn->read_ahead = READ_AHEAD_CRLF;
        goto retry;
      } else {
        /* found the end of a line, keep read_ahead for the next line */
        conn->read_ahead = read_ahead;
        break;
      }
    }

    if (G_LIKELY (*idx < size - 1))
      buffer[(*idx)++] = c;
  }
  buffer[*idx] = '\0';

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_write:
 * @conn: a #GstRTSPConnection
 * @data: the data to write
 * @size: the size of @data
 * @timeout: a timeout value or %NULL
 *
 * Attempt to write @size bytes of @data to the connected @conn, blocking up to
 * the specified @timeout. @timeout can be %NULL, in which case this function
 * might block forever.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_write (GstRTSPConnection * conn, const guint8 * data,
    guint size, GTimeVal * timeout)
{
  guint offset;
  GstClockTime to;
  GstRTSPResult res;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (data != NULL || size == 0, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->output_stream != NULL, GST_RTSP_EINVAL);

  offset = 0;

  to = timeout ? GST_TIMEVAL_TO_TIME (*timeout) : 0;

  g_socket_set_timeout (conn->write_socket, (to + GST_SECOND - 1) / GST_SECOND);
  res =
      write_bytes (conn->output_stream, data, &offset, size, TRUE,
      conn->cancellable);
  g_socket_set_timeout (conn->write_socket, 0);

  return res;
}

static GString *
message_to_string (GstRTSPConnection * conn, GstRTSPMessage * message)
{
  GString *str = NULL;

  str = g_string_new ("");

  switch (message->type) {
    case GST_RTSP_MESSAGE_REQUEST:
      /* create request string, add CSeq */
      g_string_append_printf (str, "%s %s RTSP/%s\r\n"
          "CSeq: %d\r\n",
          gst_rtsp_method_as_text (message->type_data.request.method),
          message->type_data.request.uri,
          gst_rtsp_version_as_text (message->type_data.request.version),
          conn->cseq++);
      /* add session id if we have one */
      if (conn->session_id[0] != '\0') {
        gst_rtsp_message_remove_header (message, GST_RTSP_HDR_SESSION, -1);
        gst_rtsp_message_add_header (message, GST_RTSP_HDR_SESSION,
            conn->session_id);
      }
      /* add any authentication headers */
      add_auth_header (conn, message);
      break;
    case GST_RTSP_MESSAGE_RESPONSE:
      /* create response string */
      g_string_append_printf (str, "RTSP/%s %d %s\r\n",
          gst_rtsp_version_as_text (message->type_data.response.version),
          message->type_data.response.code, message->type_data.response.reason);
      break;
    case GST_RTSP_MESSAGE_HTTP_REQUEST:
      /* create request string */
      g_string_append_printf (str, "%s %s HTTP/%s\r\n",
          gst_rtsp_method_as_text (message->type_data.request.method),
          message->type_data.request.uri,
          gst_rtsp_version_as_text (message->type_data.request.version));
      /* add any authentication headers */
      add_auth_header (conn, message);
      break;
    case GST_RTSP_MESSAGE_HTTP_RESPONSE:
      /* create response string */
      g_string_append_printf (str, "HTTP/%s %d %s\r\n",
          gst_rtsp_version_as_text (message->type_data.request.version),
          message->type_data.response.code, message->type_data.response.reason);
      break;
    case GST_RTSP_MESSAGE_DATA:
    {
      guint8 data_header[4];

      /* prepare data header */
      data_header[0] = '$';
      data_header[1] = message->type_data.data.channel;
      data_header[2] = (message->body_size >> 8) & 0xff;
      data_header[3] = message->body_size & 0xff;

      /* create string with header and data */
      str = g_string_append_len (str, (gchar *) data_header, 4);
      str =
          g_string_append_len (str, (gchar *) message->body,
          message->body_size);
      break;
    }
    default:
      g_string_free (str, TRUE);
      g_return_val_if_reached (NULL);
      break;
  }

  /* append headers and body */
  if (message->type != GST_RTSP_MESSAGE_DATA) {
    gchar date_string[100];

    gen_date_string (date_string, sizeof (date_string));

    /* add date header */
    gst_rtsp_message_remove_header (message, GST_RTSP_HDR_DATE, -1);
    gst_rtsp_message_add_header (message, GST_RTSP_HDR_DATE, date_string);

    /* append headers */
    gst_rtsp_message_append_headers (message, str);

    /* append Content-Length and body if needed */
    if (message->body != NULL && message->body_size > 0) {
      gchar *len;

      len = g_strdup_printf ("%d", message->body_size);
      g_string_append_printf (str, "%s: %s\r\n",
          gst_rtsp_header_as_text (GST_RTSP_HDR_CONTENT_LENGTH), len);
      g_free (len);
      /* header ends here */
      g_string_append (str, "\r\n");
      str =
          g_string_append_len (str, (gchar *) message->body,
          message->body_size);
    } else {
      /* just end headers */
      g_string_append (str, "\r\n");
    }
  }

  return str;
}

/**
 * gst_rtsp_connection_send:
 * @conn: a #GstRTSPConnection
 * @message: the message to send
 * @timeout: a timeout value or %NULL
 *
 * Attempt to send @message to the connected @conn, blocking up to
 * the specified @timeout. @timeout can be %NULL, in which case this function
 * might block forever.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_send (GstRTSPConnection * conn, GstRTSPMessage * message,
    GTimeVal * timeout)
{
  GString *string = NULL;
  GstRTSPResult res;
  gchar *str;
  gsize len;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);

  if (G_UNLIKELY (!(string = message_to_string (conn, message))))
    goto no_message;

  if (conn->tunneled) {
    str = g_base64_encode ((const guchar *) string->str, string->len);
    g_string_free (string, TRUE);
    len = strlen (str);
  } else {
    str = string->str;
    len = string->len;
    g_string_free (string, FALSE);
  }

  /* write request */
  res = gst_rtsp_connection_write (conn, (guint8 *) str, len, timeout);

  g_free (str);

  return res;

no_message:
  {
    g_warning ("Wrong message");
    return GST_RTSP_EINVAL;
  }
}

static GstRTSPResult
parse_string (gchar * dest, gint size, gchar ** src)
{
  GstRTSPResult res = GST_RTSP_OK;
  gint idx;

  idx = 0;
  /* skip spaces */
  while (g_ascii_isspace (**src))
    (*src)++;

  while (!g_ascii_isspace (**src) && **src != '\0') {
    if (idx < size - 1)
      dest[idx++] = **src;
    else
      res = GST_RTSP_EPARSE;
    (*src)++;
  }
  if (size > 0)
    dest[idx] = '\0';

  return res;
}

static GstRTSPResult
parse_protocol_version (gchar * protocol, GstRTSPMsgType * type,
    GstRTSPVersion * version)
{
  GstRTSPVersion rversion;
  GstRTSPResult res = GST_RTSP_OK;
  gchar *ver;

  if (G_LIKELY ((ver = strchr (protocol, '/')) != NULL)) {
    guint major;
    guint minor;
    gchar dummychar;

    *ver++ = '\0';

    /* the version number must be formatted as X.Y with nothing following */
    if (sscanf (ver, "%u.%u%c", &major, &minor, &dummychar) != 2)
      res = GST_RTSP_EPARSE;

    rversion = major * 0x10 + minor;
    if (g_ascii_strcasecmp (protocol, "RTSP") == 0) {

      if (rversion != GST_RTSP_VERSION_1_0 && rversion != GST_RTSP_VERSION_2_0) {
        *version = GST_RTSP_VERSION_INVALID;
        res = GST_RTSP_ERROR;
      }
    } else if (g_ascii_strcasecmp (protocol, "HTTP") == 0) {
      if (*type == GST_RTSP_MESSAGE_REQUEST)
        *type = GST_RTSP_MESSAGE_HTTP_REQUEST;
      else if (*type == GST_RTSP_MESSAGE_RESPONSE)
        *type = GST_RTSP_MESSAGE_HTTP_RESPONSE;

      if (rversion != GST_RTSP_VERSION_1_0 &&
          rversion != GST_RTSP_VERSION_1_1 && rversion != GST_RTSP_VERSION_2_0)
        res = GST_RTSP_ERROR;
    } else
      res = GST_RTSP_EPARSE;
  } else
    res = GST_RTSP_EPARSE;

  if (res == GST_RTSP_OK)
    *version = rversion;

  return res;
}

static GstRTSPResult
parse_response_status (guint8 * buffer, GstRTSPMessage * msg)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPResult res2;
  gchar versionstr[20];
  gchar codestr[4];
  gint code;
  gchar *bptr;

  bptr = (gchar *) buffer;

  if (parse_string (versionstr, sizeof (versionstr), &bptr) != GST_RTSP_OK)
    res = GST_RTSP_EPARSE;

  if (parse_string (codestr, sizeof (codestr), &bptr) != GST_RTSP_OK)
    res = GST_RTSP_EPARSE;
  code = atoi (codestr);
  if (G_UNLIKELY (*codestr == '\0' || code < 0 || code >= 600))
    res = GST_RTSP_EPARSE;

  while (g_ascii_isspace (*bptr))
    bptr++;

  if (G_UNLIKELY (gst_rtsp_message_init_response (msg, code, bptr,
              NULL) != GST_RTSP_OK))
    res = GST_RTSP_EPARSE;

  res2 = parse_protocol_version (versionstr, &msg->type,
      &msg->type_data.response.version);
  if (G_LIKELY (res == GST_RTSP_OK))
    res = res2;

  return res;
}

static GstRTSPResult
parse_request_line (guint8 * buffer, GstRTSPMessage * msg)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPResult res2;
  gchar versionstr[20];
  gchar methodstr[20];
  gchar urlstr[4096];
  gchar *bptr;
  GstRTSPMethod method;

  bptr = (gchar *) buffer;

  if (parse_string (methodstr, sizeof (methodstr), &bptr) != GST_RTSP_OK)
    res = GST_RTSP_EPARSE;
  method = gst_rtsp_find_method (methodstr);

  if (parse_string (urlstr, sizeof (urlstr), &bptr) != GST_RTSP_OK)
    res = GST_RTSP_EPARSE;
  if (G_UNLIKELY (*urlstr == '\0'))
    res = GST_RTSP_EPARSE;

  if (parse_string (versionstr, sizeof (versionstr), &bptr) != GST_RTSP_OK)
    res = GST_RTSP_EPARSE;

  if (G_UNLIKELY (*bptr != '\0'))
    res = GST_RTSP_EPARSE;

  if (G_UNLIKELY (gst_rtsp_message_init_request (msg, method,
              urlstr) != GST_RTSP_OK))
    res = GST_RTSP_EPARSE;

  res2 = parse_protocol_version (versionstr, &msg->type,
      &msg->type_data.request.version);
  if (G_LIKELY (res == GST_RTSP_OK))
    res = res2;

  if (G_LIKELY (msg->type == GST_RTSP_MESSAGE_REQUEST)) {
    /* GET and POST are not allowed as RTSP methods */
    if (msg->type_data.request.method == GST_RTSP_GET ||
        msg->type_data.request.method == GST_RTSP_POST) {
      msg->type_data.request.method = GST_RTSP_INVALID;
      if (res == GST_RTSP_OK)
        res = GST_RTSP_ERROR;
    }
  } else if (msg->type == GST_RTSP_MESSAGE_HTTP_REQUEST) {
    /* only GET and POST are allowed as HTTP methods */
    if (msg->type_data.request.method != GST_RTSP_GET &&
        msg->type_data.request.method != GST_RTSP_POST) {
      msg->type_data.request.method = GST_RTSP_INVALID;
      if (res == GST_RTSP_OK)
        res = GST_RTSP_ERROR;
    }
  }

  return res;
}

/* parsing lines means reading a Key: Value pair */
static GstRTSPResult
parse_line (guint8 * buffer, GstRTSPMessage * msg)
{
  GstRTSPHeaderField field;
  gchar *line = (gchar *) buffer;
  gchar *field_name = NULL;
  gchar *value;

  if ((value = strchr (line, ':')) == NULL || value == line)
    goto parse_error;

  /* trim space before the colon */
  if (value[-1] == ' ')
    value[-1] = '\0';

  /* replace the colon with a NUL */
  *value++ = '\0';

  /* find the header */
  field = gst_rtsp_find_header_field (line);
  /* custom header not present in the list of pre-defined headers */
  if (field == GST_RTSP_HDR_INVALID)
    field_name = line;

  /* split up the value in multiple key:value pairs if it contains comma(s) */
  while (*value != '\0') {
    gchar *next_value;
    gchar *comma = NULL;
    gboolean quoted = FALSE;
    guint comment = 0;

    /* trim leading space */
    if (*value == ' ')
      value++;

    /* for headers which may not appear multiple times, and thus may not
     * contain multiple values on the same line, we can short-circuit the loop
     * below and the entire value results in just one key:value pair*/
    if (!gst_rtsp_header_allow_multiple (field))
      next_value = value + strlen (value);
    else
      next_value = value;

    /* find the next value, taking special care of quotes and comments */
    while (*next_value != '\0') {
      if ((quoted || comment != 0) && *next_value == '\\' &&
          next_value[1] != '\0')
        next_value++;
      else if (comment == 0 && *next_value == '"')
        quoted = !quoted;
      else if (!quoted && *next_value == '(')
        comment++;
      else if (comment != 0 && *next_value == ')')
        comment--;
      else if (!quoted && comment == 0) {
        /* To quote RFC 2068: "User agents MUST take special care in parsing
         * the WWW-Authenticate field value if it contains more than one
         * challenge, or if more than one WWW-Authenticate header field is
         * provided, since the contents of a challenge may itself contain a
         * comma-separated list of authentication parameters."
         *
         * What this means is that we cannot just look for an unquoted comma
         * when looking for multiple values in Proxy-Authenticate and
         * WWW-Authenticate headers. Instead we need to look for the sequence
         * "comma [space] token space token" before we can split after the
         * comma...
         */
        if (field == GST_RTSP_HDR_PROXY_AUTHENTICATE ||
            field == GST_RTSP_HDR_WWW_AUTHENTICATE) {
          if (*next_value == ',') {
            if (next_value[1] == ' ') {
              /* skip any space following the comma so we do not mistake it for
               * separating between two tokens */
              next_value++;
            }
            comma = next_value;
          } else if (*next_value == ' ' && next_value[1] != ',' &&
              next_value[1] != '=' && comma != NULL) {
            next_value = comma;
            comma = NULL;
            break;
          }
        } else if (*next_value == ',')
          break;
      }

      next_value++;
    }

    if (msg->type == GST_RTSP_MESSAGE_REQUEST && field == GST_RTSP_HDR_SESSION) {
      /* The timeout parameter is only allowed in a session response header
       * but some clients send it as part of the session request header.
       * Ignore everything from the semicolon to the end of the line. */
      next_value = value;
      while (*next_value != '\0') {
        if (*next_value == ';') {
          break;
        }
        next_value++;
      }
    }

    /* trim space */
    if (value != next_value && next_value[-1] == ' ')
      next_value[-1] = '\0';

    if (*next_value != '\0')
      *next_value++ = '\0';

    /* add the key:value pair */
    if (*value != '\0') {
      if (field != GST_RTSP_HDR_INVALID)
        gst_rtsp_message_add_header (msg, field, value);
      else
        gst_rtsp_message_add_header_by_name (msg, field_name, value);
    }

    value = next_value;
  }

  return GST_RTSP_OK;

  /* ERRORS */
parse_error:
  {
    return GST_RTSP_EPARSE;
  }
}

/* convert all consecutive whitespace to a single space */
static void
normalize_line (guint8 * buffer)
{
  while (*buffer) {
    if (g_ascii_isspace (*buffer)) {
      guint8 *tmp;

      *buffer++ = ' ';
      for (tmp = buffer; g_ascii_isspace (*tmp); tmp++) {
      }
      if (buffer != tmp)
        memmove (buffer, tmp, strlen ((gchar *) tmp) + 1);
    } else {
      buffer++;
    }
  }
}

/* returns:
 *  GST_RTSP_OK when a complete message was read.
 *  GST_RTSP_EEOF: when the read socket is closed
 *  GST_RTSP_EINTR: when more data is needed.
 *  GST_RTSP_..: some other error occured.
 */
static GstRTSPResult
build_next (GstRTSPBuilder * builder, GstRTSPMessage * message,
    GstRTSPConnection * conn, gboolean block)
{
  GstRTSPResult res;

  while (TRUE) {
    switch (builder->state) {
      case STATE_START:
      {
        guint8 c;

        builder->offset = 0;
        res =
            read_bytes (conn, (guint8 *) builder->buffer, &builder->offset, 1,
            block);
        if (res != GST_RTSP_OK)
          goto done;

        c = builder->buffer[0];

        /* we have 1 bytes now and we can see if this is a data message or
         * not */
        if (c == '$') {
          /* data message, prepare for the header */
          builder->state = STATE_DATA_HEADER;
          conn->may_cancel = FALSE;
        } else if (c == '\n' || c == '\r') {
          /* skip \n and \r */
          builder->offset = 0;
        } else {
          builder->line = 0;
          builder->state = STATE_READ_LINES;
          conn->may_cancel = FALSE;
        }
        break;
      }
      case STATE_DATA_HEADER:
      {
        res =
            read_bytes (conn, (guint8 *) builder->buffer, &builder->offset, 4,
            block);
        if (res != GST_RTSP_OK)
          goto done;

        gst_rtsp_message_init_data (message, builder->buffer[1]);

        builder->body_len = (builder->buffer[2] << 8) | builder->buffer[3];
        builder->body_data = g_malloc (builder->body_len + 1);
        builder->body_data[builder->body_len] = '\0';
        builder->offset = 0;
        builder->state = STATE_DATA_BODY;
        break;
      }
      case STATE_DATA_BODY:
      {
        res =
            read_bytes (conn, builder->body_data, &builder->offset,
            builder->body_len, block);
        if (res != GST_RTSP_OK)
          goto done;

        /* we have the complete body now, store in the message adjusting the
         * length to include the trailing '\0' */
        gst_rtsp_message_take_body (message,
            (guint8 *) builder->body_data, builder->body_len + 1);
        builder->body_data = NULL;
        builder->body_len = 0;

        builder->state = STATE_END;
        break;
      }
      case STATE_READ_LINES:
      {
        res = read_line (conn, builder->buffer, &builder->offset,
            sizeof (builder->buffer), block);
        if (res != GST_RTSP_OK)
          goto done;

        /* we have a regular response */
        if (builder->buffer[0] == '\0') {
          gchar *hdrval;

          /* empty line, end of message header */
          /* see if there is a Content-Length header, but ignore it if this
           * is a POST request with an x-sessioncookie header */
          if (gst_rtsp_message_get_header (message,
                  GST_RTSP_HDR_CONTENT_LENGTH, &hdrval, 0) == GST_RTSP_OK &&
              (message->type != GST_RTSP_MESSAGE_HTTP_REQUEST ||
                  message->type_data.request.method != GST_RTSP_POST ||
                  gst_rtsp_message_get_header (message,
                      GST_RTSP_HDR_X_SESSIONCOOKIE, NULL, 0) != GST_RTSP_OK)) {
            /* there is, prepare to read the body */
            builder->body_len = atol (hdrval);
            builder->body_data = g_try_malloc (builder->body_len + 1);
            /* we can't do much here, we need the length to know how many bytes
             * we need to read next and when allocation fails, something is
             * probably wrong with the length. */
            if (builder->body_data == NULL)
              goto invalid_body_len;

            builder->body_data[builder->body_len] = '\0';
            builder->offset = 0;
            builder->state = STATE_DATA_BODY;
          } else {
            builder->state = STATE_END;
          }
          break;
        }

        /* we have a line */
        normalize_line (builder->buffer);
        if (builder->line == 0) {
          /* first line, check for response status */
          if (memcmp (builder->buffer, "RTSP", 4) == 0 ||
              memcmp (builder->buffer, "HTTP", 4) == 0) {
            builder->status = parse_response_status (builder->buffer, message);
          } else {
            builder->status = parse_request_line (builder->buffer, message);
          }
        } else {
          /* else just parse the line */
          res = parse_line (builder->buffer, message);
          if (res != GST_RTSP_OK)
            builder->status = res;
        }
        builder->line++;
        builder->offset = 0;
        break;
      }
      case STATE_END:
      {
        gchar *session_cookie;
        gchar *session_id;

        conn->may_cancel = TRUE;

        if (message->type == GST_RTSP_MESSAGE_DATA) {
          /* data messages don't have headers */
          res = GST_RTSP_OK;
          goto done;
        }

        /* save the tunnel session in the connection */
        if (message->type == GST_RTSP_MESSAGE_HTTP_REQUEST &&
            !conn->manual_http &&
            conn->tstate == TUNNEL_STATE_NONE &&
            gst_rtsp_message_get_header (message, GST_RTSP_HDR_X_SESSIONCOOKIE,
                &session_cookie, 0) == GST_RTSP_OK) {
          strncpy (conn->tunnelid, session_cookie, TUNNELID_LEN);
          conn->tunnelid[TUNNELID_LEN - 1] = '\0';
          conn->tunneled = TRUE;
        }

        /* save session id in the connection for further use */
        if (message->type == GST_RTSP_MESSAGE_RESPONSE &&
            gst_rtsp_message_get_header (message, GST_RTSP_HDR_SESSION,
                &session_id, 0) == GST_RTSP_OK) {
          gint maxlen, i;

          maxlen = sizeof (conn->session_id) - 1;
          /* the sessionid can have attributes marked with ;
           * Make sure we strip them */
          for (i = 0; session_id[i] != '\0'; i++) {
            if (session_id[i] == ';') {
              maxlen = i;
              /* parse timeout */
              do {
                i++;
              } while (g_ascii_isspace (session_id[i]));
              if (g_str_has_prefix (&session_id[i], "timeout=")) {
                gint to;

                /* if we parsed something valid, configure */
                if ((to = atoi (&session_id[i + 8])) > 0)
                  conn->timeout = to;
              }
              break;
            }
          }

          /* make sure to not overflow */
          if (conn->remember_session_id) {
            strncpy (conn->session_id, session_id, maxlen);
            conn->session_id[maxlen] = '\0';
          }
        }
        res = builder->status;
        goto done;
      }
      default:
        res = GST_RTSP_ERROR;
        break;
    }
  }
done:
  conn->may_cancel = TRUE;
  return res;

  /* ERRORS */
invalid_body_len:
  {
    conn->may_cancel = TRUE;
    GST_DEBUG ("could not allocate body");
    return GST_RTSP_ERROR;
  }
}

/**
 * gst_rtsp_connection_read:
 * @conn: a #GstRTSPConnection
 * @data: the data to read
 * @size: the size of @data
 * @timeout: a timeout value or %NULL
 *
 * Attempt to read @size bytes into @data from the connected @conn, blocking up to
 * the specified @timeout. @timeout can be %NULL, in which case this function
 * might block forever.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_read (GstRTSPConnection * conn, guint8 * data, guint size,
    GTimeVal * timeout)
{
  guint offset;
  GstClockTime to;
  GstRTSPResult res;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (data != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->read_socket != NULL, GST_RTSP_EINVAL);

  if (G_UNLIKELY (size == 0))
    return GST_RTSP_OK;

  offset = 0;

  /* configure timeout if any */
  to = timeout ? GST_TIMEVAL_TO_TIME (*timeout) : 0;

  g_socket_set_timeout (conn->read_socket, (to + GST_SECOND - 1) / GST_SECOND);
  res = read_bytes (conn, data, &offset, size, TRUE);
  g_socket_set_timeout (conn->read_socket, 0);

  return res;
}

static GstRTSPMessage *
gen_tunnel_reply (GstRTSPConnection * conn, GstRTSPStatusCode code,
    const GstRTSPMessage * request)
{
  GstRTSPMessage *msg;
  GstRTSPResult res;

  if (gst_rtsp_status_as_text (code) == NULL)
    code = GST_RTSP_STS_INTERNAL_SERVER_ERROR;

  GST_RTSP_CHECK (gst_rtsp_message_new_response (&msg, code, NULL, request),
      no_message);

  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_SERVER,
      "GStreamer RTSP Server");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CONNECTION, "close");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CACHE_CONTROL, "no-store");
  gst_rtsp_message_add_header (msg, GST_RTSP_HDR_PRAGMA, "no-cache");

  if (code == GST_RTSP_STS_OK) {
    /* add the local ip address to the tunnel reply, this is where the client
     * should send the POST request to */
    if (conn->local_ip)
      gst_rtsp_message_add_header (msg, GST_RTSP_HDR_X_SERVER_IP_ADDRESS,
          conn->local_ip);
    gst_rtsp_message_add_header (msg, GST_RTSP_HDR_CONTENT_TYPE,
        "application/x-rtsp-tunnelled");
  }

  return msg;

  /* ERRORS */
no_message:
  {
    return NULL;
  }
}

/**
 * gst_rtsp_connection_receive:
 * @conn: a #GstRTSPConnection
 * @message: the message to read
 * @timeout: a timeout value or %NULL
 *
 * Attempt to read into @message from the connected @conn, blocking up to
 * the specified @timeout. @timeout can be %NULL, in which case this function
 * might block forever.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_receive (GstRTSPConnection * conn, GstRTSPMessage * message,
    GTimeVal * timeout)
{
  GstRTSPResult res;
  GstRTSPBuilder builder;
  GstClockTime to;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->read_socket != NULL, GST_RTSP_EINVAL);

  /* configure timeout if any */
  to = timeout ? GST_TIMEVAL_TO_TIME (*timeout) : 0;

  g_socket_set_timeout (conn->read_socket, (to + GST_SECOND - 1) / GST_SECOND);
  memset (&builder, 0, sizeof (GstRTSPBuilder));
  res = build_next (&builder, message, conn, TRUE);
  g_socket_set_timeout (conn->read_socket, 0);

  if (G_UNLIKELY (res != GST_RTSP_OK))
    goto read_error;

  if (!conn->manual_http) {
    if (message->type == GST_RTSP_MESSAGE_HTTP_REQUEST) {
      if (conn->tstate == TUNNEL_STATE_NONE &&
          message->type_data.request.method == GST_RTSP_GET) {
        GstRTSPMessage *response;

        conn->tstate = TUNNEL_STATE_GET;

        /* tunnel GET request, we can reply now */
        response = gen_tunnel_reply (conn, GST_RTSP_STS_OK, message);
        res = gst_rtsp_connection_send (conn, response, timeout);
        gst_rtsp_message_free (response);
        if (res == GST_RTSP_OK)
          res = GST_RTSP_ETGET;
        goto cleanup;
      } else if (conn->tstate == TUNNEL_STATE_NONE &&
          message->type_data.request.method == GST_RTSP_POST) {
        conn->tstate = TUNNEL_STATE_POST;

        /* tunnel POST request, the caller now has to link the two
         * connections. */
        res = GST_RTSP_ETPOST;
        goto cleanup;
      } else {
        res = GST_RTSP_EPARSE;
        goto cleanup;
      }
    } else if (message->type == GST_RTSP_MESSAGE_HTTP_RESPONSE) {
      res = GST_RTSP_EPARSE;
      goto cleanup;
    }
  }

  /* we have a message here */
  build_reset (&builder);

  return GST_RTSP_OK;

  /* ERRORS */
read_error:
cleanup:
  {
    build_reset (&builder);
    gst_rtsp_message_unset (message);
    return res;
  }
}

/**
 * gst_rtsp_connection_close:
 * @conn: a #GstRTSPConnection
 *
 * Close the connected @conn. After this call, the connection is in the same
 * state as when it was first created.
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_close (GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  /* last unref closes the connection we don't want to explicitly close here
   * because these sockets might have been provided at construction */
  if (conn->stream0) {
    g_object_unref (conn->stream0);
    conn->stream0 = NULL;
    conn->socket0 = NULL;
  }
  if (conn->stream1) {
    g_object_unref (conn->stream1);
    conn->stream1 = NULL;
    conn->socket1 = NULL;
  }

  /* these were owned by the stream */
  conn->input_stream = NULL;
  conn->output_stream = NULL;
  conn->control_stream = NULL;

  g_free (conn->remote_ip);
  conn->remote_ip = NULL;
  g_free (conn->local_ip);
  conn->local_ip = NULL;

  conn->read_ahead = 0;

  g_free (conn->initial_buffer);
  conn->initial_buffer = NULL;
  conn->initial_buffer_offset = 0;

  conn->write_socket = NULL;
  conn->read_socket = NULL;
  conn->tunneled = FALSE;
  conn->tstate = TUNNEL_STATE_NONE;
  conn->ctxp = NULL;
  g_free (conn->username);
  conn->username = NULL;
  g_free (conn->passwd);
  conn->passwd = NULL;
  gst_rtsp_connection_clear_auth_params (conn);
  conn->timeout = 60;
  conn->cseq = 0;
  conn->session_id[0] = '\0';

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_free:
 * @conn: a #GstRTSPConnection
 *
 * Close and free @conn.
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_free (GstRTSPConnection * conn)
{
  GstRTSPResult res;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  res = gst_rtsp_connection_close (conn);

  if (conn->cancellable)
    g_object_unref (conn->cancellable);
  if (conn->client)
    g_object_unref (conn->client);
  if (conn->tls_database)
    g_object_unref (conn->tls_database);
  if (conn->tls_interaction)
    g_object_unref (conn->tls_interaction);
  if (conn->accept_certificate_destroy_notify)
    conn->
        accept_certificate_destroy_notify (conn->accept_certificate_user_data);

  g_timer_destroy (conn->timer);
  gst_rtsp_url_free (conn->url);
  g_free (conn->proxy_host);
  g_free (conn);

  return res;
}

/**
 * gst_rtsp_connection_poll:
 * @conn: a #GstRTSPConnection
 * @events: a bitmask of #GstRTSPEvent flags to check
 * @revents: location for result flags
 * @timeout: a timeout
 *
 * Wait up to the specified @timeout for the connection to become available for
 * at least one of the operations specified in @events. When the function returns
 * with #GST_RTSP_OK, @revents will contain a bitmask of available operations on
 * @conn.
 *
 * @timeout can be %NULL, in which case this function might block forever.
 *
 * This function can be cancelled with gst_rtsp_connection_flush().
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_poll (GstRTSPConnection * conn, GstRTSPEvent events,
    GstRTSPEvent * revents, GTimeVal * timeout)
{
  GMainContext *ctx;
  GSource *rs, *ws, *ts;
  GIOCondition condition;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (events != 0, GST_RTSP_EINVAL);
  g_return_val_if_fail (revents != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->read_socket != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->write_socket != NULL, GST_RTSP_EINVAL);

  ctx = g_main_context_new ();

  /* configure timeout if any */
  if (timeout) {
    ts = g_timeout_source_new (GST_TIMEVAL_TO_TIME (*timeout) / GST_MSECOND);
    g_source_set_dummy_callback (ts);
    g_source_attach (ts, ctx);
    g_source_unref (ts);
  }

  if (events & GST_RTSP_EV_READ) {
    rs = g_socket_create_source (conn->read_socket, G_IO_IN | G_IO_PRI,
        conn->cancellable);
    g_source_set_dummy_callback (rs);
    g_source_attach (rs, ctx);
    g_source_unref (rs);
  }

  if (events & GST_RTSP_EV_WRITE) {
    ws = g_socket_create_source (conn->write_socket, G_IO_OUT,
        conn->cancellable);
    g_source_set_dummy_callback (ws);
    g_source_attach (ws, ctx);
    g_source_unref (ws);
  }

  /* Returns after handling all pending events */
  while (!g_main_context_iteration (ctx, TRUE));

  g_main_context_unref (ctx);

  *revents = 0;
  if (events & GST_RTSP_EV_READ) {
    condition = g_socket_condition_check (conn->read_socket,
        G_IO_IN | G_IO_PRI);
    if ((condition & G_IO_IN) || (condition & G_IO_PRI))
      *revents |= GST_RTSP_EV_READ;
  }
  if (events & GST_RTSP_EV_WRITE) {
    condition = g_socket_condition_check (conn->write_socket, G_IO_OUT);
    if ((condition & G_IO_OUT))
      *revents |= GST_RTSP_EV_WRITE;
  }

  if (*revents == 0)
    return GST_RTSP_ETIMEOUT;

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_next_timeout:
 * @conn: a #GstRTSPConnection
 * @timeout: a timeout
 *
 * Calculate the next timeout for @conn, storing the result in @timeout.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_connection_next_timeout (GstRTSPConnection * conn, GTimeVal * timeout)
{
  gdouble elapsed;
  glong sec;
  gulong usec;
  gint ctimeout;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (timeout != NULL, GST_RTSP_EINVAL);

  ctimeout = conn->timeout;
  if (ctimeout >= 20) {
    /* Because we should act before the timeout we timeout 5
     * seconds in advance. */
    ctimeout -= 5;
  } else if (ctimeout >= 5) {
    /* else timeout 20% earlier */
    ctimeout -= ctimeout / 5;
  } else if (ctimeout >= 1) {
    /* else timeout 1 second earlier */
    ctimeout -= 1;
  }

  elapsed = g_timer_elapsed (conn->timer, &usec);
  if (elapsed >= ctimeout) {
    sec = 0;
    usec = 0;
  } else {
    sec = ctimeout - elapsed;
    if (usec <= G_USEC_PER_SEC)
      usec = G_USEC_PER_SEC - usec;
    else
      usec = 0;
  }

  timeout->tv_sec = sec;
  timeout->tv_usec = usec;

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_reset_timeout:
 * @conn: a #GstRTSPConnection
 *
 * Reset the timeout of @conn.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_connection_reset_timeout (GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  g_timer_start (conn->timer);

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_flush:
 * @conn: a #GstRTSPConnection
 * @flush: start or stop the flush
 *
 * Start or stop the flushing action on @conn. When flushing, all current
 * and future actions on @conn will return #GST_RTSP_EINTR until the connection
 * is set to non-flushing mode again.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_connection_flush (GstRTSPConnection * conn, gboolean flush)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  if (flush) {
    g_cancellable_cancel (conn->cancellable);
  } else {
    g_object_unref (conn->cancellable);
    conn->cancellable = g_cancellable_new ();
  }

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_set_proxy:
 * @conn: a #GstRTSPConnection
 * @host: the proxy host
 * @port: the proxy port
 *
 * Set the proxy host and port.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_connection_set_proxy (GstRTSPConnection * conn,
    const gchar * host, guint port)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  g_free (conn->proxy_host);
  conn->proxy_host = g_strdup (host);
  conn->proxy_port = port;

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_set_auth:
 * @conn: a #GstRTSPConnection
 * @method: authentication method
 * @user: the user
 * @pass: the password
 *
 * Configure @conn for authentication mode @method with @user and @pass as the
 * user and password respectively.
 *
 * Returns: #GST_RTSP_OK.
 */
GstRTSPResult
gst_rtsp_connection_set_auth (GstRTSPConnection * conn,
    GstRTSPAuthMethod method, const gchar * user, const gchar * pass)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  if (method == GST_RTSP_AUTH_DIGEST && ((user == NULL || pass == NULL)
          || g_strrstr (user, ":") != NULL))
    return GST_RTSP_EINVAL;

  /* Make sure the username and passwd are being set for authentication */
  if (method == GST_RTSP_AUTH_NONE && (user == NULL || pass == NULL))
    return GST_RTSP_EINVAL;

  /* ":" chars are not allowed in usernames for basic auth */
  if (method == GST_RTSP_AUTH_BASIC && g_strrstr (user, ":") != NULL)
    return GST_RTSP_EINVAL;

  g_free (conn->username);
  g_free (conn->passwd);

  conn->auth_method = method;
  conn->username = g_strdup (user);
  conn->passwd = g_strdup (pass);

  return GST_RTSP_OK;
}

/**
 * str_case_hash:
 * @key: ASCII string to hash
 *
 * Hashes @key in a case-insensitive manner.
 *
 * Returns: the hash code.
 **/
static guint
str_case_hash (gconstpointer key)
{
  const char *p = key;
  guint h = g_ascii_toupper (*p);

  if (h)
    for (p += 1; *p != '\0'; p++)
      h = (h << 5) - h + g_ascii_toupper (*p);

  return h;
}

/**
 * str_case_equal:
 * @v1: an ASCII string
 * @v2: another ASCII string
 *
 * Compares @v1 and @v2 in a case-insensitive manner
 *
 * Returns: %TRUE if they are equal (modulo case)
 **/
static gboolean
str_case_equal (gconstpointer v1, gconstpointer v2)
{
  const char *string1 = v1;
  const char *string2 = v2;

  return g_ascii_strcasecmp (string1, string2) == 0;
}

/**
 * gst_rtsp_connection_set_auth_param:
 * @conn: a #GstRTSPConnection
 * @param: authentication directive
 * @value: value
 *
 * Setup @conn with authentication directives. This is not necesary for
 * methods #GST_RTSP_AUTH_NONE and #GST_RTSP_AUTH_BASIC. For
 * #GST_RTSP_AUTH_DIGEST, directives should be taken from the digest challenge
 * in the WWW-Authenticate response header and can include realm, domain,
 * nonce, opaque, stale, algorithm, qop as per RFC2617.
 */
void
gst_rtsp_connection_set_auth_param (GstRTSPConnection * conn,
    const gchar * param, const gchar * value)
{
  g_return_if_fail (conn != NULL);
  g_return_if_fail (param != NULL);

  if (conn->auth_params == NULL) {
    conn->auth_params =
        g_hash_table_new_full (str_case_hash, str_case_equal, g_free, g_free);
  }
  g_hash_table_insert (conn->auth_params, g_strdup (param), g_strdup (value));
}

/**
 * gst_rtsp_connection_clear_auth_params:
 * @conn: a #GstRTSPConnection
 *
 * Clear the list of authentication directives stored in @conn.
 */
void
gst_rtsp_connection_clear_auth_params (GstRTSPConnection * conn)
{
  g_return_if_fail (conn != NULL);

  if (conn->auth_params != NULL) {
    g_hash_table_destroy (conn->auth_params);
    conn->auth_params = NULL;
  }
}

static GstRTSPResult
set_qos_dscp (GSocket * socket, guint qos_dscp)
{
#ifndef IP_TOS
  GST_FIXME ("IP_TOS socket option is not defined, not setting dscp");
  return GST_RTSP_OK;
#else
  gint fd;
  union gst_sockaddr sa;
  socklen_t slen = sizeof (sa);
  gint af;
  gint tos;

  if (!socket)
    return GST_RTSP_OK;

  fd = g_socket_get_fd (socket);
  if (getsockname (fd, &sa.sa, &slen) < 0)
    goto no_getsockname;

  af = sa.sa.sa_family;

  /* if this is an IPv4-mapped address then do IPv4 QoS */
  if (af == AF_INET6) {
    if (IN6_IS_ADDR_V4MAPPED (&sa.sa_in6.sin6_addr))
      af = AF_INET;
  }

  /* extract and shift 6 bits of the DSCP */
  tos = (qos_dscp & 0x3f) << 2;

#ifdef G_OS_WIN32
#  define SETSOCKOPT_ARG4_TYPE const char *
#else
#  define SETSOCKOPT_ARG4_TYPE const void *
#endif

  switch (af) {
    case AF_INET:
      if (setsockopt (fd, IPPROTO_IP, IP_TOS, (SETSOCKOPT_ARG4_TYPE) & tos,
              sizeof (tos)) < 0)
        goto no_setsockopt;
      break;
    case AF_INET6:
#ifdef IPV6_TCLASS
      if (setsockopt (fd, IPPROTO_IPV6, IPV6_TCLASS,
              (SETSOCKOPT_ARG4_TYPE) & tos, sizeof (tos)) < 0)
        goto no_setsockopt;
      break;
#endif
    default:
      goto wrong_family;
  }

  return GST_RTSP_OK;

  /* ERRORS */
no_getsockname:
no_setsockopt:
  {
    return GST_RTSP_ESYS;
  }
wrong_family:
  {
    return GST_RTSP_ERROR;
  }
#endif
}

/**
 * gst_rtsp_connection_set_qos_dscp:
 * @conn: a #GstRTSPConnection
 * @qos_dscp: DSCP value
 *
 * Configure @conn to use the specified DSCP value.
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_set_qos_dscp (GstRTSPConnection * conn, guint qos_dscp)
{
  GstRTSPResult res;

  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->read_socket != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (conn->write_socket != NULL, GST_RTSP_EINVAL);

  res = set_qos_dscp (conn->socket0, qos_dscp);
  if (res == GST_RTSP_OK)
    res = set_qos_dscp (conn->socket1, qos_dscp);

  return res;
}


/**
 * gst_rtsp_connection_get_url:
 * @conn: a #GstRTSPConnection
 *
 * Retrieve the URL of the other end of @conn.
 *
 * Returns: The URL. This value remains valid until the
 * connection is freed.
 */
GstRTSPUrl *
gst_rtsp_connection_get_url (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, NULL);

  return conn->url;
}

/**
 * gst_rtsp_connection_get_ip:
 * @conn: a #GstRTSPConnection
 *
 * Retrieve the IP address of the other end of @conn.
 *
 * Returns: The IP address as a string. this value remains valid until the
 * connection is closed.
 */
const gchar *
gst_rtsp_connection_get_ip (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, NULL);

  return conn->remote_ip;
}

/**
 * gst_rtsp_connection_set_ip:
 * @conn: a #GstRTSPConnection
 * @ip: an ip address
 *
 * Set the IP address of the server.
 */
void
gst_rtsp_connection_set_ip (GstRTSPConnection * conn, const gchar * ip)
{
  g_return_if_fail (conn != NULL);

  g_free (conn->remote_ip);
  conn->remote_ip = g_strdup (ip);
}

/**
 * gst_rtsp_connection_get_read_socket:
 * @conn: a #GstRTSPConnection
 *
 * Get the file descriptor for reading.
 *
 * Returns: (transfer none): the file descriptor used for reading or %NULL on
 * error. The file descriptor remains valid until the connection is closed.
 */
GSocket *
gst_rtsp_connection_get_read_socket (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, NULL);
  g_return_val_if_fail (conn->read_socket != NULL, NULL);

  return conn->read_socket;
}

/**
 * gst_rtsp_connection_get_write_socket:
 * @conn: a #GstRTSPConnection
 *
 * Get the file descriptor for writing.
 *
 * Returns: (transfer none): the file descriptor used for writing or NULL on
 * error. The file descriptor remains valid until the connection is closed.
 */
GSocket *
gst_rtsp_connection_get_write_socket (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, NULL);
  g_return_val_if_fail (conn->write_socket != NULL, NULL);

  return conn->write_socket;
}

/**
 * gst_rtsp_connection_set_http_mode:
 * @conn: a #GstRTSPConnection
 * @enable: %TRUE to enable manual HTTP mode
 *
 * By setting the HTTP mode to %TRUE the message parsing will support HTTP
 * messages in addition to the RTSP messages. It will also disable the
 * automatic handling of setting up an HTTP tunnel.
 */
void
gst_rtsp_connection_set_http_mode (GstRTSPConnection * conn, gboolean enable)
{
  g_return_if_fail (conn != NULL);

  conn->manual_http = enable;
}

/**
 * gst_rtsp_connection_set_tunneled:
 * @conn: a #GstRTSPConnection
 * @tunneled: the new state
 *
 * Set the HTTP tunneling state of the connection. This must be configured before
 * the @conn is connected.
 */
void
gst_rtsp_connection_set_tunneled (GstRTSPConnection * conn, gboolean tunneled)
{
  g_return_if_fail (conn != NULL);
  g_return_if_fail (conn->read_socket == NULL);
  g_return_if_fail (conn->write_socket == NULL);

  conn->tunneled = tunneled;
}

/**
 * gst_rtsp_connection_is_tunneled:
 * @conn: a #GstRTSPConnection
 *
 * Get the tunneling state of the connection.
 *
 * Returns: if @conn is using HTTP tunneling.
 */
gboolean
gst_rtsp_connection_is_tunneled (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, FALSE);

  return conn->tunneled;
}

/**
 * gst_rtsp_connection_get_tunnelid:
 * @conn: a #GstRTSPConnection
 *
 * Get the tunnel session id the connection.
 *
 * Returns: returns a non-empty string if @conn is being tunneled over HTTP.
 */
const gchar *
gst_rtsp_connection_get_tunnelid (const GstRTSPConnection * conn)
{
  g_return_val_if_fail (conn != NULL, NULL);

  if (!conn->tunneled)
    return NULL;

  return conn->tunnelid;
}

/**
 * gst_rtsp_connection_do_tunnel:
 * @conn: a #GstRTSPConnection
 * @conn2: a #GstRTSPConnection or %NULL
 *
 * If @conn received the first tunnel connection and @conn2 received
 * the second tunnel connection, link the two connections together so that
 * @conn manages the tunneled connection.
 *
 * After this call, @conn2 cannot be used anymore and must be freed with
 * gst_rtsp_connection_free().
 *
 * If @conn2 is %NULL then only the base64 decoding context will be setup for
 * @conn.
 *
 * Returns: return GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_connection_do_tunnel (GstRTSPConnection * conn,
    GstRTSPConnection * conn2)
{
  g_return_val_if_fail (conn != NULL, GST_RTSP_EINVAL);

  if (conn2 != NULL) {
    GstRTSPTunnelState ts1 = conn->tstate;
    GstRTSPTunnelState ts2 = conn2->tstate;

    g_return_val_if_fail ((ts1 == TUNNEL_STATE_GET && ts2 == TUNNEL_STATE_POST)
        || (ts1 == TUNNEL_STATE_POST && ts2 == TUNNEL_STATE_GET),
        GST_RTSP_EINVAL);
    g_return_val_if_fail (!memcmp (conn2->tunnelid, conn->tunnelid,
            TUNNELID_LEN), GST_RTSP_EINVAL);

    /* both connections have socket0 as the read/write socket */
    if (ts1 == TUNNEL_STATE_GET) {
      /* conn2 is the HTTP POST channel. take its socket and set it as read
       * socket in conn */
      conn->socket1 = conn2->socket0;
      conn->stream1 = conn2->stream0;
      conn->input_stream = conn2->input_stream;
      conn->control_stream = g_io_stream_get_input_stream (conn->stream0);
      conn2->output_stream = NULL;
    } else {
      /* conn2 is the HTTP GET channel. take its socket and set it as write
       * socket in conn */
      conn->socket1 = conn->socket0;
      conn->stream1 = conn->stream0;
      conn->socket0 = conn2->socket0;
      conn->stream0 = conn2->stream0;
      conn->output_stream = conn2->output_stream;
      conn->control_stream = g_io_stream_get_input_stream (conn->stream0);
    }

    /* clean up some of the state of conn2 */
    g_cancellable_cancel (conn2->cancellable);
    conn2->write_socket = conn2->read_socket = NULL;
    conn2->socket0 = NULL;
    conn2->stream0 = NULL;
    conn2->socket1 = NULL;
    conn2->stream1 = NULL;
    conn2->input_stream = NULL;
    conn2->control_stream = NULL;
    g_object_unref (conn2->cancellable);
    conn2->cancellable = NULL;

    /* We make socket0 the write socket and socket1 the read socket. */
    conn->write_socket = conn->socket0;
    conn->read_socket = conn->socket1;

    conn->tstate = TUNNEL_STATE_COMPLETE;

    g_free (conn->initial_buffer);
    conn->initial_buffer = conn2->initial_buffer;
    conn2->initial_buffer = NULL;
    conn->initial_buffer_offset = conn2->initial_buffer_offset;
  }

  /* we need base64 decoding for the readfd */
  conn->ctx.state = 0;
  conn->ctx.save = 0;
  conn->ctx.cout = 0;
  conn->ctx.coutl = 0;
  conn->ctxp = &conn->ctx;

  return GST_RTSP_OK;
}

/**
 * gst_rtsp_connection_set_remember_session_id:
 * @conn: a #GstRTSPConnection
 * @remember: %TRUE if the connection should remember the session id
 *
 * Sets if the #GstRTSPConnection should remember the session id from the last
 * response received and force it onto any further requests.
 *
 * The default value is %TRUE
 */

void
gst_rtsp_connection_set_remember_session_id (GstRTSPConnection * conn,
    gboolean remember)
{
  conn->remember_session_id = remember;
  if (!remember)
    conn->session_id[0] = '\0';
}

/**
 * gst_rtsp_connection_get_remember_session_id:
 * @conn: a #GstRTSPConnection
 *
 * Returns: %TRUE if the #GstRTSPConnection remembers the session id in the
 * last response to set it on any further request.
 */

gboolean
gst_rtsp_connection_get_remember_session_id (GstRTSPConnection * conn)
{
  return conn->remember_session_id;
}


#define READ_ERR    (G_IO_HUP | G_IO_ERR | G_IO_NVAL)
#define READ_COND   (G_IO_IN | READ_ERR)
#define WRITE_ERR   (G_IO_HUP | G_IO_ERR | G_IO_NVAL)
#define WRITE_COND  (G_IO_OUT | WRITE_ERR)

typedef struct
{
  guint8 *data;
  guint size;
  guint id;
} GstRTSPRec;

/* async functions */
struct _GstRTSPWatch
{
  GSource source;

  GstRTSPConnection *conn;

  GstRTSPBuilder builder;
  GstRTSPMessage message;

  GSource *readsrc;
  GSource *writesrc;
  GSource *controlsrc;

  gboolean keep_running;

  /* queued message for transmission */
  guint id;
  GMutex mutex;
  GstQueueArray *messages;
  gsize messages_bytes;
  guint8 *write_data;
  guint write_off;
  guint write_size;
  guint write_id;
  gsize max_bytes;
  guint max_messages;
  GCond queue_not_full;
  gboolean flushing;

  GstRTSPWatchFuncs funcs;

  gpointer user_data;
  GDestroyNotify notify;
};

#define IS_BACKLOG_FULL(w) (((w)->max_bytes != 0 && (w)->messages_bytes >= (w)->max_bytes) || \
      ((w)->max_messages != 0 && gst_queue_array_get_length((w)->messages) >= (w)->max_messages))

static gboolean
gst_rtsp_source_prepare (GSource * source, gint * timeout)
{
  GstRTSPWatch *watch = (GstRTSPWatch *) source;

  if (watch->conn->initial_buffer != NULL)
    return TRUE;

  *timeout = (watch->conn->timeout * 1000);

  return FALSE;
}

static gboolean
gst_rtsp_source_check (GSource * source)
{
  return FALSE;
}

static gboolean
gst_rtsp_source_dispatch_read_get_channel (GPollableInputStream * stream,
    GstRTSPWatch * watch)
{
  gssize count;
  guint8 buffer[1024];
  GError *error = NULL;

  /* try to read in order to be able to detect errors, we read 1k in case some
   * client actually decides to send data on the GET channel */
  count = g_pollable_input_stream_read_nonblocking (stream, buffer, 1024, NULL,
      &error);
  if (count == 0) {
    /* other end closed the socket */
    goto eof;
  }

  if (count < 0) {
    GST_DEBUG ("%s", error->message);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK) ||
        g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
      g_clear_error (&error);
      goto done;
    }
    g_clear_error (&error);
    goto read_error;
  }

  /* client sent data on the GET channel, ignore it */

done:
  return TRUE;

  /* ERRORS */
eof:
  {
    if (watch->funcs.closed)
      watch->funcs.closed (watch, watch->user_data);

    /* the read connection was closed, stop the watch now */
    watch->keep_running = FALSE;

    return FALSE;
  }
read_error:
  {
    if (watch->funcs.error_full)
      watch->funcs.error_full (watch, GST_RTSP_ESYS, &watch->message,
          0, watch->user_data);
    else if (watch->funcs.error)
      watch->funcs.error (watch, GST_RTSP_ESYS, watch->user_data);

    goto eof;
  }
}

static gboolean
gst_rtsp_source_dispatch_read (GPollableInputStream * stream,
    GstRTSPWatch * watch)
{
  GstRTSPResult res = GST_RTSP_ERROR;
  GstRTSPConnection *conn = watch->conn;

  /* if this connection was already closed, stop now */
  if (G_POLLABLE_INPUT_STREAM (conn->input_stream) != stream)
    goto eof;

  res = build_next (&watch->builder, &watch->message, conn, FALSE);
  if (res == GST_RTSP_EINTR)
    goto done;
  else if (G_UNLIKELY (res == GST_RTSP_EEOF)) {
    g_mutex_lock (&watch->mutex);
    if (watch->readsrc) {
      if (!g_source_is_destroyed ((GSource *) watch))
        g_source_remove_child_source ((GSource *) watch, watch->readsrc);
      g_source_unref (watch->readsrc);
      watch->readsrc = NULL;
    }

    if (conn->stream1) {
      g_object_unref (conn->stream1);
      conn->stream1 = NULL;
      conn->socket1 = NULL;
      conn->input_stream = NULL;
    }
    g_mutex_unlock (&watch->mutex);

    /* When we are in tunnelled mode, the read socket can be closed and we
     * should be prepared for a new POST method to reopen it */
    if (conn->tstate == TUNNEL_STATE_COMPLETE) {
      /* remove the read connection for the tunnel */
      /* we accept a new POST request */
      conn->tstate = TUNNEL_STATE_GET;
      /* and signal that we lost our tunnel */
      if (watch->funcs.tunnel_lost)
        res = watch->funcs.tunnel_lost (watch, watch->user_data);
      /* we add read source on the write socket able to detect when client closes get channel in tunneled mode */
      g_mutex_lock (&watch->mutex);
      if (watch->conn->control_stream && !watch->controlsrc) {
        watch->controlsrc =
            g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM
            (watch->conn->control_stream), NULL);
        g_source_set_callback (watch->controlsrc,
            (GSourceFunc) gst_rtsp_source_dispatch_read_get_channel, watch,
            NULL);
        g_source_add_child_source ((GSource *) watch, watch->controlsrc);
      }
      g_mutex_unlock (&watch->mutex);
      goto read_done;
    } else
      goto eof;
  } else if (G_LIKELY (res == GST_RTSP_OK)) {
    if (!conn->manual_http &&
        watch->message.type == GST_RTSP_MESSAGE_HTTP_REQUEST) {
      if (conn->tstate == TUNNEL_STATE_NONE &&
          watch->message.type_data.request.method == GST_RTSP_GET) {
        GstRTSPMessage *response;
        GstRTSPStatusCode code;

        conn->tstate = TUNNEL_STATE_GET;

        if (watch->funcs.tunnel_start)
          code = watch->funcs.tunnel_start (watch, watch->user_data);
        else
          code = GST_RTSP_STS_OK;

        /* queue the response */
        response = gen_tunnel_reply (conn, code, &watch->message);
        if (watch->funcs.tunnel_http_response)
          watch->funcs.tunnel_http_response (watch, &watch->message, response,
              watch->user_data);
        gst_rtsp_watch_send_message (watch, response, NULL);
        gst_rtsp_message_free (response);
        goto read_done;
      } else if (conn->tstate == TUNNEL_STATE_NONE &&
          watch->message.type_data.request.method == GST_RTSP_POST) {
        conn->tstate = TUNNEL_STATE_POST;

        /* in the callback the connection should be tunneled with the
         * GET connection */
        if (watch->funcs.tunnel_complete) {
          watch->funcs.tunnel_complete (watch, watch->user_data);
        }
        goto read_done;
      }
    }
  } else
    goto read_error;

  if (!conn->manual_http) {
    /* if manual HTTP support is not enabled, then restore the message to
     * what it would have looked like without the support for parsing HTTP
     * messages being present */
    if (watch->message.type == GST_RTSP_MESSAGE_HTTP_REQUEST) {
      watch->message.type = GST_RTSP_MESSAGE_REQUEST;
      watch->message.type_data.request.method = GST_RTSP_INVALID;
      if (watch->message.type_data.request.version != GST_RTSP_VERSION_1_0)
        watch->message.type_data.request.version = GST_RTSP_VERSION_INVALID;
      res = GST_RTSP_EPARSE;
    } else if (watch->message.type == GST_RTSP_MESSAGE_HTTP_RESPONSE) {
      watch->message.type = GST_RTSP_MESSAGE_RESPONSE;
      if (watch->message.type_data.response.version != GST_RTSP_VERSION_1_0)
        watch->message.type_data.response.version = GST_RTSP_VERSION_INVALID;
      res = GST_RTSP_EPARSE;
    }
  }
  if (G_LIKELY (res != GST_RTSP_OK))
    goto read_error;

  if (watch->funcs.message_received)
    watch->funcs.message_received (watch, &watch->message, watch->user_data);

read_done:
  gst_rtsp_message_unset (&watch->message);
  build_reset (&watch->builder);

done:
  return TRUE;

  /* ERRORS */
eof:
  {
    if (watch->funcs.closed)
      watch->funcs.closed (watch, watch->user_data);

    /* we closed the read connection, stop the watch now */
    watch->keep_running = FALSE;

    /* always stop when the input returns EOF in non-tunneled mode */
    return FALSE;
  }
read_error:
  {
    if (watch->funcs.error_full)
      watch->funcs.error_full (watch, res, &watch->message,
          0, watch->user_data);
    else if (watch->funcs.error)
      watch->funcs.error (watch, res, watch->user_data);

    goto eof;
  }
}

static gboolean
gst_rtsp_source_dispatch (GSource * source, GSourceFunc callback G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  GstRTSPWatch *watch = (GstRTSPWatch *) source;
  GstRTSPConnection *conn = watch->conn;

  if (conn->initial_buffer != NULL) {
    gst_rtsp_source_dispatch_read (G_POLLABLE_INPUT_STREAM (conn->input_stream),
        watch);
  }
  return watch->keep_running;
}

static gboolean
gst_rtsp_source_dispatch_write (GPollableOutputStream * stream,
    GstRTSPWatch * watch)
{
  GstRTSPResult res = GST_RTSP_ERROR;
  GstRTSPConnection *conn = watch->conn;

  /* if this connection was already closed, stop now */
  if (G_POLLABLE_OUTPUT_STREAM (conn->output_stream) != stream)
    goto eof;

  g_mutex_lock (&watch->mutex);
  do {
    if (watch->write_data == NULL) {
      GstRTSPRec *rec_ptr, rec;

      /* get a new message from the queue */
      rec_ptr = gst_queue_array_pop_head_struct (watch->messages);
      if (rec_ptr == NULL) {
        if (watch->writesrc) {
          if (!g_source_is_destroyed ((GSource *) watch))
            g_source_remove_child_source ((GSource *) watch, watch->writesrc);
          g_source_unref (watch->writesrc);
          watch->writesrc = NULL;
          /* we create and add the write source again when we actually have
           * something to write */

          /* since write source is now removed we add read source on the write
           * socket instead to be able to detect when client closes get channel
           * in tunneled mode */
          if (watch->conn->control_stream) {
            watch->controlsrc =
                g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM
                (watch->conn->control_stream), NULL);
            g_source_set_callback (watch->controlsrc,
                (GSourceFunc) gst_rtsp_source_dispatch_read_get_channel, watch,
                NULL);
            g_source_add_child_source ((GSource *) watch, watch->controlsrc);
          } else {
            watch->controlsrc = NULL;
          }
        }
        break;
      }

      rec = *rec_ptr;
      watch->messages_bytes -= rec.size;

      watch->write_off = 0;
      watch->write_data = rec.data;
      watch->write_size = rec.size;
      watch->write_id = rec.id;
    }

    res = write_bytes (conn->output_stream, watch->write_data,
        &watch->write_off, watch->write_size, FALSE, conn->cancellable);

    if (!IS_BACKLOG_FULL (watch))
      g_cond_signal (&watch->queue_not_full);
    g_mutex_unlock (&watch->mutex);

    if (res == GST_RTSP_EINTR)
      goto write_blocked;
    else if (G_LIKELY (res == GST_RTSP_OK)) {
      if (watch->funcs.message_sent)
        watch->funcs.message_sent (watch, watch->write_id, watch->user_data);
    } else {
      goto write_error;
    }
    g_mutex_lock (&watch->mutex);

    g_free (watch->write_data);
    watch->write_data = NULL;
  } while (TRUE);
  g_mutex_unlock (&watch->mutex);

write_blocked:
  return TRUE;

  /* ERRORS */
eof:
  {
    return FALSE;
  }
write_error:
  {
    if (watch->funcs.error_full)
      watch->funcs.error_full (watch, res, NULL,
          watch->write_id, watch->user_data);
    else if (watch->funcs.error)
      watch->funcs.error (watch, res, watch->user_data);

    return FALSE;
  }
}

static void
gst_rtsp_rec_clear (gpointer data)
{
  GstRTSPRec *rec = data;

  g_free (rec->data);
}

static void
gst_rtsp_source_finalize (GSource * source)
{
  GstRTSPWatch *watch = (GstRTSPWatch *) source;
  GstRTSPRec *rec;

  if (watch->notify)
    watch->notify (watch->user_data);

  build_reset (&watch->builder);
  gst_rtsp_message_unset (&watch->message);

  while ((rec = gst_queue_array_pop_head_struct (watch->messages)))
    gst_rtsp_rec_clear (rec);
  gst_queue_array_free (watch->messages);
  watch->messages = NULL;
  watch->messages_bytes = 0;

  g_free (watch->write_data);
  g_cond_clear (&watch->queue_not_full);

  if (watch->readsrc)
    g_source_unref (watch->readsrc);
  if (watch->writesrc)
    g_source_unref (watch->writesrc);
  if (watch->controlsrc)
    g_source_unref (watch->controlsrc);

  g_mutex_clear (&watch->mutex);
}

static GSourceFuncs gst_rtsp_source_funcs = {
  gst_rtsp_source_prepare,
  gst_rtsp_source_check,
  gst_rtsp_source_dispatch,
  gst_rtsp_source_finalize,
  NULL,
  NULL
};

/**
 * gst_rtsp_watch_new: (skip)
 * @conn: a #GstRTSPConnection
 * @funcs: watch functions
 * @user_data: user data to pass to @funcs
 * @notify: notify when @user_data is not referenced anymore
 *
 * Create a watch object for @conn. The functions provided in @funcs will be
 * called with @user_data when activity happened on the watch.
 *
 * The new watch is usually created so that it can be attached to a
 * maincontext with gst_rtsp_watch_attach().
 *
 * @conn must exist for the entire lifetime of the watch.
 *
 * Returns: a #GstRTSPWatch that can be used for asynchronous RTSP
 * communication. Free with gst_rtsp_watch_unref () after usage.
 */
GstRTSPWatch *
gst_rtsp_watch_new (GstRTSPConnection * conn,
    GstRTSPWatchFuncs * funcs, gpointer user_data, GDestroyNotify notify)
{
  GstRTSPWatch *result;

  g_return_val_if_fail (conn != NULL, NULL);
  g_return_val_if_fail (funcs != NULL, NULL);
  g_return_val_if_fail (conn->read_socket != NULL, NULL);
  g_return_val_if_fail (conn->write_socket != NULL, NULL);

  result = (GstRTSPWatch *) g_source_new (&gst_rtsp_source_funcs,
      sizeof (GstRTSPWatch));

  result->conn = conn;
  result->builder.state = STATE_START;

  g_mutex_init (&result->mutex);
  result->messages = gst_queue_array_new_for_struct (sizeof (GstRTSPRec), 10);
  g_cond_init (&result->queue_not_full);

  gst_rtsp_watch_reset (result);
  result->keep_running = TRUE;
  result->flushing = FALSE;

  result->funcs = *funcs;
  result->user_data = user_data;
  result->notify = notify;

  return result;
}

/**
 * gst_rtsp_watch_reset:
 * @watch: a #GstRTSPWatch
 *
 * Reset @watch, this is usually called after gst_rtsp_connection_do_tunnel()
 * when the file descriptors of the connection might have changed.
 */
void
gst_rtsp_watch_reset (GstRTSPWatch * watch)
{
  g_mutex_lock (&watch->mutex);
  if (watch->readsrc) {
    g_source_remove_child_source ((GSource *) watch, watch->readsrc);
    g_source_unref (watch->readsrc);
  }
  if (watch->writesrc) {
    g_source_remove_child_source ((GSource *) watch, watch->writesrc);
    g_source_unref (watch->writesrc);
    watch->writesrc = NULL;
  }
  if (watch->controlsrc) {
    g_source_remove_child_source ((GSource *) watch, watch->controlsrc);
    g_source_unref (watch->controlsrc);
    watch->controlsrc = NULL;
  }

  if (watch->conn->input_stream) {
    watch->readsrc =
        g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM
        (watch->conn->input_stream), NULL);
    g_source_set_callback (watch->readsrc,
        (GSourceFunc) gst_rtsp_source_dispatch_read, watch, NULL);
    g_source_add_child_source ((GSource *) watch, watch->readsrc);
  } else {
    watch->readsrc = NULL;
  }

  /* we create and add the write source when we actually have something to
   * write */

  /* when write source is not added we add read source on the write socket
   * instead to be able to detect when client closes get channel in tunneled
   * mode */
  if (watch->conn->control_stream) {
    watch->controlsrc =
        g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM
        (watch->conn->control_stream), NULL);
    g_source_set_callback (watch->controlsrc,
        (GSourceFunc) gst_rtsp_source_dispatch_read_get_channel, watch, NULL);
    g_source_add_child_source ((GSource *) watch, watch->controlsrc);
  } else {
    watch->controlsrc = NULL;
  }
  g_mutex_unlock (&watch->mutex);
}

/**
 * gst_rtsp_watch_attach:
 * @watch: a #GstRTSPWatch
 * @context: a GMainContext (if NULL, the default context will be used)
 *
 * Adds a #GstRTSPWatch to a context so that it will be executed within that context.
 *
 * Returns: the ID (greater than 0) for the watch within the GMainContext.
 */
guint
gst_rtsp_watch_attach (GstRTSPWatch * watch, GMainContext * context)
{
  g_return_val_if_fail (watch != NULL, 0);

  return g_source_attach ((GSource *) watch, context);
}

/**
 * gst_rtsp_watch_unref:
 * @watch: a #GstRTSPWatch
 *
 * Decreases the reference count of @watch by one. If the resulting reference
 * count is zero the watch and associated memory will be destroyed.
 */
void
gst_rtsp_watch_unref (GstRTSPWatch * watch)
{
  g_return_if_fail (watch != NULL);

  g_source_unref ((GSource *) watch);
}

/**
 * gst_rtsp_watch_set_send_backlog:
 * @watch: a #GstRTSPWatch
 * @bytes: maximum bytes
 * @messages: maximum messages
 *
 * Set the maximum amount of bytes and messages that will be queued in @watch.
 * When the maximum amounts are exceeded, gst_rtsp_watch_write_data() and
 * gst_rtsp_watch_send_message() will return #GST_RTSP_ENOMEM.
 *
 * A value of 0 for @bytes or @messages means no limits.
 *
 * Since: 1.2
 */
void
gst_rtsp_watch_set_send_backlog (GstRTSPWatch * watch,
    gsize bytes, guint messages)
{
  g_return_if_fail (watch != NULL);

  g_mutex_lock (&watch->mutex);
  watch->max_bytes = bytes;
  watch->max_messages = messages;
  if (!IS_BACKLOG_FULL (watch))
    g_cond_signal (&watch->queue_not_full);
  g_mutex_unlock (&watch->mutex);

  GST_DEBUG ("set backlog to bytes %" G_GSIZE_FORMAT ", messages %u",
      bytes, messages);
}

/**
 * gst_rtsp_watch_get_send_backlog:
 * @watch: a #GstRTSPWatch
 * @bytes: (out) (allow-none): maximum bytes
 * @messages: (out) (allow-none): maximum messages
 *
 * Get the maximum amount of bytes and messages that will be queued in @watch.
 * See gst_rtsp_watch_set_send_backlog().
 *
 * Since: 1.2
 */
void
gst_rtsp_watch_get_send_backlog (GstRTSPWatch * watch,
    gsize * bytes, guint * messages)
{
  g_return_if_fail (watch != NULL);

  g_mutex_lock (&watch->mutex);
  if (bytes)
    *bytes = watch->max_bytes;
  if (messages)
    *messages = watch->max_messages;
  g_mutex_unlock (&watch->mutex);
}

/**
 * gst_rtsp_watch_write_data:
 * @watch: a #GstRTSPWatch
 * @data: (array length=size) (transfer full): the data to queue
 * @size: the size of @data
 * @id: (out) (allow-none): location for a message ID or %NULL
 *
 * Write @data using the connection of the @watch. If it cannot be sent
 * immediately, it will be queued for transmission in @watch. The contents of
 * @message will then be serialized and transmitted when the connection of the
 * @watch becomes writable. In case the @message is queued, the ID returned in
 * @id will be non-zero and used as the ID argument in the message_sent
 * callback.
 *
 * This function will take ownership of @data and g_free() it after use.
 *
 * If the amount of queued data exceeds the limits set with
 * gst_rtsp_watch_set_send_backlog(), this function will return
 * #GST_RTSP_ENOMEM.
 *
 * Returns: #GST_RTSP_OK on success. #GST_RTSP_ENOMEM when the backlog limits
 * are reached. #GST_RTSP_EINTR when @watch was flushing.
 */
GstRTSPResult
gst_rtsp_watch_write_data (GstRTSPWatch * watch, const guint8 * data,
    guint size, guint * id)
{
  GstRTSPResult res;
  GstRTSPRec rec;
  guint off = 0;
  GMainContext *context = NULL;

  g_return_val_if_fail (watch != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (data != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (size != 0, GST_RTSP_EINVAL);

  g_mutex_lock (&watch->mutex);
  if (watch->flushing)
    goto flushing;

  /* try to send the message synchronously first */
  if (gst_queue_array_get_length (watch->messages) == 0
      && watch->write_data == NULL) {
    res =
        write_bytes (watch->conn->output_stream, data, &off, size, FALSE,
        watch->conn->cancellable);
    if (res != GST_RTSP_EINTR) {
      if (id != NULL)
        *id = 0;
      g_free ((gpointer) data);
      goto done;
    }
  }

  /* check limits */
  if (IS_BACKLOG_FULL (watch))
    goto too_much_backlog;

  /* make a record with the data and id for sending async */
  memset (&rec, 0, sizeof (rec));
  if (off == 0) {
    rec.data = (guint8 *) data;
    rec.size = size;
  } else {
    rec.data = g_memdup (data + off, size - off);
    rec.size = size - off;
    g_free ((gpointer) data);
  }

  do {
    /* make sure rec->id is never 0 */
    rec.id = ++watch->id;
  } while (G_UNLIKELY (rec.id == 0));

  /* add the record to a queue. */
  gst_queue_array_push_tail_struct (watch->messages, &rec);
  watch->messages_bytes += rec.size;

  /* make sure the main context will now also check for writability on the
   * socket */
  context = ((GSource *) watch)->context;
  if (!watch->writesrc) {
    /* remove the read source on the write socket, we will be able to detect
     * errors while writing */
    if (watch->controlsrc) {
      g_source_remove_child_source ((GSource *) watch, watch->controlsrc);
      g_source_unref (watch->controlsrc);
      watch->controlsrc = NULL;
    }

    watch->writesrc =
        g_pollable_output_stream_create_source (G_POLLABLE_OUTPUT_STREAM
        (watch->conn->output_stream), NULL);
    g_source_set_callback (watch->writesrc,
        (GSourceFunc) gst_rtsp_source_dispatch_write, watch, NULL);
    g_source_add_child_source ((GSource *) watch, watch->writesrc);
  }

  if (id != NULL)
    *id = rec.id;
  res = GST_RTSP_OK;

done:
  g_mutex_unlock (&watch->mutex);

  if (context)
    g_main_context_wakeup (context);

  return res;

  /* ERRORS */
flushing:
  {
    GST_DEBUG ("we are flushing");
    g_mutex_unlock (&watch->mutex);
    g_free ((gpointer) data);
    return GST_RTSP_EINTR;
  }
too_much_backlog:
  {
    GST_WARNING ("too much backlog: max_bytes %" G_GSIZE_FORMAT ", current %"
        G_GSIZE_FORMAT ", max_messages %u, current %u", watch->max_bytes,
        watch->messages_bytes, watch->max_messages,
        gst_queue_array_get_length (watch->messages));
    g_mutex_unlock (&watch->mutex);
    g_free ((gpointer) data);
    return GST_RTSP_ENOMEM;
  }
}

/**
 * gst_rtsp_watch_send_message:
 * @watch: a #GstRTSPWatch
 * @message: a #GstRTSPMessage
 * @id: (out) (allow-none): location for a message ID or %NULL
 *
 * Send a @message using the connection of the @watch. If it cannot be sent
 * immediately, it will be queued for transmission in @watch. The contents of
 * @message will then be serialized and transmitted when the connection of the
 * @watch becomes writable. In case the @message is queued, the ID returned in
 * @id will be non-zero and used as the ID argument in the message_sent
 * callback.
 *
 * Returns: #GST_RTSP_OK on success.
 */
GstRTSPResult
gst_rtsp_watch_send_message (GstRTSPWatch * watch, GstRTSPMessage * message,
    guint * id)
{
  GString *str;
  guint size;

  g_return_val_if_fail (watch != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);

  /* make a record with the message as a string and id */
  str = message_to_string (watch->conn, message);
  size = str->len;
  return gst_rtsp_watch_write_data (watch,
      (guint8 *) g_string_free (str, FALSE), size, id);
}

/**
 * gst_rtsp_watch_wait_backlog:
 * @watch: a #GstRTSPWatch
 * @timeout: a #GTimeVal timeout
 *
 * Wait until there is place in the backlog queue, @timeout is reached
 * or @watch is set to flushing.
 *
 * If @timeout is %NULL this function can block forever. If @timeout
 * contains a valid timeout, this function will return %GST_RTSP_ETIMEOUT
 * after the timeout expired.
 *
 * The typically use of this function is when gst_rtsp_watch_write_data
 * returns %GST_RTSP_ENOMEM. The caller then calls this function to wait for
 * free space in the backlog queue and try again.
 *
 * Returns: %GST_RTSP_OK when if there is room in queue.
 *          %GST_RTSP_ETIMEOUT when @timeout was reached.
 *          %GST_RTSP_EINTR when @watch is flushing
 *          %GST_RTSP_EINVAL when called with invalid parameters.
 *
 * Since: 1.4
 */
GstRTSPResult
gst_rtsp_watch_wait_backlog (GstRTSPWatch * watch, GTimeVal * timeout)
{
  gint64 end_time;
  GstClockTime to;

  g_return_val_if_fail (watch != NULL, GST_RTSP_EINVAL);

  to = timeout ? GST_TIMEVAL_TO_TIME (*timeout) : 0;
  end_time = g_get_monotonic_time () + GST_TIME_AS_USECONDS (to);

  g_mutex_lock (&watch->mutex);
  if (watch->flushing)
    goto flushing;

  while (IS_BACKLOG_FULL (watch)) {
    gboolean res;

    res = g_cond_wait_until (&watch->queue_not_full, &watch->mutex, end_time);
    if (watch->flushing)
      goto flushing;

    if (!res)
      goto timeout;
  }
  g_mutex_unlock (&watch->mutex);

  return GST_RTSP_OK;

  /* ERRORS */
flushing:
  {
    GST_DEBUG ("we are flushing");
    g_mutex_unlock (&watch->mutex);
    return GST_RTSP_EINTR;
  }
timeout:
  {
    GST_DEBUG ("we timed out");
    g_mutex_unlock (&watch->mutex);
    return GST_RTSP_ETIMEOUT;
  }
}

/**
 * gst_rtsp_watch_set_flushing:
 * @watch: a #GstRTSPWatch
 * @flushing: new flushing state
 *
 * When @flushing is %TRUE, abort a call to gst_rtsp_watch_wait_backlog()
 * and make sure gst_rtsp_watch_write_data() returns immediately with
 * #GST_RTSP_EINTR. And empty the queue.
 *
 * Since: 1.4
 */
void
gst_rtsp_watch_set_flushing (GstRTSPWatch * watch, gboolean flushing)
{
  g_return_if_fail (watch != NULL);

  g_mutex_lock (&watch->mutex);
  watch->flushing = flushing;
  g_cond_signal (&watch->queue_not_full);
  if (flushing) {
    GstRTSPRec *rec;

    while ((rec = gst_queue_array_pop_head_struct (watch->messages)))
      gst_rtsp_rec_clear (rec);
  }
  g_mutex_unlock (&watch->mutex);
}
