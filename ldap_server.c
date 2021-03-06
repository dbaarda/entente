/*=
 * Copyright (c) 2017 Donovan Baarda <abo@minkirri.apana.org.au>
 * Based on entente Copyright (c) 2010, 2011 Sergey Urbanovich
 *
 * Licensed under the GPLv3 License. See LICENSE file for details.
 */

#include "ldap_server.h"
#include "nss2ldap.h"

static const char *LDAPOID_StartTLS = "1.3.6.1.4.1.1466.20037";

void sighup_cb(ev_loop *loop, ev_signal *watcher, int revents);
void sigterm_cb(ev_loop *loop, ev_signal *watcher, int revents);
void accept_cb(ev_loop *loop, ev_io *watcher, int revents);
void read_cb(ev_loop *loop, ev_io *watcher, int revents);
void write_cb(ev_loop *loop, ev_io *watcher, int revents);
void delay_cb(EV_P_ ev_timer *w, int revents);
void handshake_cb(ev_loop *loop, ev_io *watcher, int revents);
void goodbye_cb(ev_loop *loop, ev_io *watcher, int revents);

int ldap_server_init(ldap_server *server, ev_loop *loop, const char *basedn, const char *rootuser, const bool anonok,
                     const char *crtpath, const char *caspath, const char *keypath, const ldap_ranges *uids,
                     const ldap_ranges *gids)
{
    mbedtls_net_init(&server->socket);
    server->basedn = basedn;
    server->rootuser = rootuser;
    /* We set rootuid from rootuser later in ldap_server_start(). */
    server->rootuid = 0;
    server->anonok = anonok;
    server->uids = uids;
    server->gids = gids;
    server->loop = loop;
    ev_signal_init(&server->sighup_watcher, sighup_cb, SIGHUP);
    server->sighup_watcher.data = server;
    ev_signal_init(&server->sigint_watcher, sigterm_cb, SIGINT);
    server->sigint_watcher.data = server;
    ev_signal_init(&server->sigterm_watcher, sigterm_cb, SIGTERM);
    server->sigterm_watcher.data = server;
    ev_init(&server->connection_watcher, accept_cb);
    server->connection_watcher.data = server;
    server->ssl = NULL;
    server->connection = NULL;
    server->cxn_opened_c = 0;
    server->cxn_closed_c = 0;
    server->msg_send_c = 0;
    server->msg_recv_c = 0;
    if (crtpath && !(server->ssl = mbedtls_ssl_server_new(crtpath, caspath, keypath)))
        return 1;
    return 0;
}

void ldap_server_start(ldap_server *server, mbedtls_net_context socket)
{
    assert(!ev_is_active(&server->sighup_watcher));
    assert(!ev_is_active(&server->sigint_watcher));
    assert(!ev_is_active(&server->sigterm_watcher));
    assert(!ev_is_active(&server->connection_watcher));

    lwarnx("server starting");
    /* We set rootuid here so it is resolved inside any chroot. */
    server->rootuid = name2uid(server->rootuser);
    server->socket = socket;
    ev_io_set(&server->connection_watcher, socket.fd, EV_READ);
    ev_io_start(server->loop, &server->connection_watcher);
    ev_signal_start(server->loop, &server->sighup_watcher);
    ev_signal_start(server->loop, &server->sigint_watcher);
    ev_signal_start(server->loop, &server->sigterm_watcher);
}

void ldap_server_stop(ldap_server *server)
{
    assert(ev_is_active(&server->sighup_watcher));
    assert(ev_is_active(&server->sigint_watcher));
    assert(ev_is_active(&server->sigterm_watcher));
    assert(ev_is_active(&server->connection_watcher));

    lwarnx("server stopping");
    /* Close all the connections. */
    for (ldap_connection *c = server->connection; c; c = ldap_connection_next(&server->connection, c))
        ldap_connection_close(c);
    ev_signal_stop(server->loop, &server->sighup_watcher);
    ev_signal_stop(server->loop, &server->sigint_watcher);
    ev_signal_stop(server->loop, &server->sigterm_watcher);
    ev_io_stop(server->loop, &server->connection_watcher);
    mbedtls_net_free(&server->socket);
}

ldap_connection *ldap_connection_new(ldap_server *server, mbedtls_net_context socket, const char *ip)
{
    ldap_connection *connection = XNEW0(ldap_connection, 1);

    connection->server = server;
    connection->id = ++server->cxn_opened_c;
    connection->socket = socket;
    strcpy(connection->client_ip, ip);
    connection->binduid = (uid_t)(-1);
    ev_io_init(&connection->read_watcher, read_cb, socket.fd, EV_READ);
    connection->read_watcher.data = connection;
    ev_io_init(&connection->write_watcher, write_cb, socket.fd, EV_WRITE);
    connection->write_watcher.data = connection;
    ev_init(&connection->delay_watcher, delay_cb);
    connection->delay_watcher.data = connection;
    connection->recv_msg = NULL;
    connection->request = NULL;
    connection->delay = 0.0;
    buffer_init(&connection->recv_buf);
    buffer_init(&connection->send_buf);
    connection->ssl = NULL;
    /* Add the connection to the server's circular dlist. */
    ldap_connection_add(&server->connection, connection);
    ev_io_start(server->loop, &connection->read_watcher);
    lcnote(connection, "new connection");
    return connection;
}

void ldap_connection_free(ldap_connection *connection)
{
    ldap_server *server = connection->server;

    lcnote(connection, "disconnected");
    /* Remove the connection from the server's circular dlist. */
    ldap_connection_rem(&server->connection, connection);
    ev_io_stop(server->loop, &connection->read_watcher);
    ev_io_stop(server->loop, &connection->write_watcher);
    ev_timer_stop(server->loop, &connection->delay_watcher);
    mbedtls_net_free(&connection->socket);
    LDAPMessage_free(connection->recv_msg);
    while (connection->request)
        ldap_request_free(connection->request);
    mbedtls_ssl_connection_free(connection->ssl);
    server->cxn_closed_c++;
    free(connection);
}

void ldap_connection_close(ldap_connection *connection)
{
    /* Change the watcher callbacks for goodbye. */
    ev_set_cb(&connection->read_watcher, goodbye_cb);
    ev_set_cb(&connection->write_watcher, goodbye_cb);
    ev_io_start(connection->server->loop, &connection->write_watcher);
}

void ldap_connection_respond(ldap_connection *connection)
{
    assert(connection);
    ldap_server *server = connection->server;
    LDAPMessage_t **msg = &connection->recv_msg;
    ldap_status_t status;

    /* While we've recieved a message, add a request. */
    while ((status = ldap_connection_recv(connection, msg)) == RC_OK) {
        switch ((*msg)->protocolOp.present) {
            /* For known request types, create a new request. */
        case LDAPMessage__protocolOp_PR_bindRequest:
            ldap_request_bind(connection, *msg);
            break;
        case LDAPMessage__protocolOp_PR_searchRequest:
            ldap_request_search(connection, *msg);
            break;
        case LDAPMessage__protocolOp_PR_abandonRequest:
            ldap_request_abandon(connection, *msg);
            break;
        case LDAPMessage__protocolOp_PR_extendedReq:
            ldap_request_extended(connection, *msg);
            break;
        case LDAPMessage__protocolOp_PR_unbindRequest:
            /* For unbindRequest, close the connection. */
            lcinfo(connection, "%ld:%s unbind request", (*msg)->messageID, LDAPMessage_name(*msg));
            return ldap_connection_close(connection);
        default:
            /* For unknown, close the connection. */
            lcwarnx(connection, "%ld:%s unknown request", (*msg)->messageID, LDAPMessage_name(*msg));
            return ldap_connection_close(connection);
        }
        *msg = NULL;
    }
    /* If we got an error receiving messages, close the connection. */
    if (status == RC_FAIL) {
        lcwarn(connection, "failure receiving message");
        return ldap_connection_close(connection);
    }
    /* While there's a request and we are not blocked, respond to the request. */
    while (connection->request && (status = ldap_request_respond(connection->request)) == RC_OK) ;
    /* If we got an error sending messages, close the connection. */
    if (status == RC_FAIL) {
        lcwarn(connection, "failure sending message");
        return ldap_connection_close(connection);
    }
    /* Update the state of all the connection watchers. */
    if (connection->delay && !ev_is_active(&connection->delay_watcher)) {
        ev_timer_set(&connection->delay_watcher, connection->delay, 0.0);
        ev_timer_start(server->loop, &connection->delay_watcher);
    }
    if (connection->delay || buffer_full(&connection->recv_buf))
        ev_io_stop(server->loop, &connection->read_watcher);
    else
        ev_io_start(server->loop, &connection->read_watcher);
    if (buffer_empty(&connection->send_buf))
        ev_io_stop(server->loop, &connection->write_watcher);
    else
        ev_io_start(server->loop, &connection->write_watcher);
}

ldap_status_t ldap_connection_send(ldap_connection *connection, LDAPMessage_t *msg)
{
    buffer_t *buf = &connection->send_buf;
    asn_enc_rval_t rencode;

    /* Send nothing if connection is delayed. */
    if (connection->delay)
        return RC_WMORE;
    /* from asn1c's FAQ: If you want BER or DER encoding, use der_encode(). */
    rencode = der_encode_to_buffer(&asn_DEF_LDAPMessage, msg, buffer_wpos(buf), buffer_wlen(buf));
    /* If it failed the buffer was full, return RC_WMORE to try again. */
    if (rencode.encoded == -1)
        return RC_WMORE;
    buffer_fill(buf, rencode.encoded);
    connection->server->msg_send_c++;
    LDAP_DEBUG(msg);
    return RC_OK;
}

ldap_status_t ldap_connection_recv(ldap_connection *connection, LDAPMessage_t **msg)
{
    buffer_t *buf = &connection->recv_buf;
    asn_dec_rval_t rdecode;

    /* Recv nothing if connection is delayed. */
    if (connection->delay)
        return RC_WMORE;
    rdecode = ber_decode(0, &asn_DEF_LDAPMessage, (void **)msg, buffer_rpos(buf), buffer_rlen(buf));
    buffer_toss(buf, rdecode.consumed);
    if (rdecode.code == RC_FAIL) {
        fail1("ber_decode", RC_FAIL);
    } else if (rdecode.code == RC_OK) {
        connection->server->msg_recv_c++;
        LDAP_DEBUG(*msg);
    }
    return rdecode.code;
}

void sighup_cb(ev_loop *loop, ev_signal *watcher, int revents)
{
    ldap_server *server = watcher->data;
    assert(server->loop == loop);
    assert(&server->sighup_watcher == watcher);
    assert(revents == EV_SIGNAL);

    lnote("SIGHUP received, reloading conf... oh wait, never mind.");
}

void sigterm_cb(ev_loop *loop, ev_signal *watcher, int revents)
{
    ldap_server *server = watcher->data;
    assert(server->loop == loop);
    assert(&server->sigint_watcher == watcher || &server->sigterm_watcher == watcher);
    assert(revents == EV_SIGNAL);

    lnote("SIGTERM received, shutting down.");
    ldap_server_stop(server);
}

void accept_cb(ev_loop *loop, ev_io *watcher, int revents)
{
    ldap_server *server = watcher->data;
    mbedtls_net_context socket;
    char addr[16];
    size_t len;
    char ip[INET6_ADDRSTRLEN];
    assert(server->loop == loop);
    assert(&server->connection_watcher == watcher);

    if (EV_ERROR & revents)
        fail("got invalid event");
    if (mbedtls_net_accept(&server->socket, &socket, addr, sizeof(addr), &len))
        fail("mbedtls_net_accept error");
    /* Set nonblock mode so mbedtls_ssl_handshake() is non-blocking. */
    if (mbedtls_net_set_nonblock(&socket)) {
        mbedtls_net_free(&socket);
        fail("mbedtls_net_set_nonblock");
    }
    if (!inet_ntop(len == 4 ? AF_INET : AF_INET6, addr, ip, sizeof(ip))) {
        lwarn("inet_ntop() failed to format client address");
        strcpy(ip, "<unknown>");
    }
    ldap_connection_new(server, socket, ip);
}

void read_cb(ev_loop *loop, ev_io *watcher, int revents)
{
    ldap_connection *connection = watcher->data;
    buffer_t *buf = &connection->recv_buf;
    ssize_t buf_cnt;
    assert(connection->server->loop == loop);
    assert(&connection->read_watcher == watcher);

    if (EV_ERROR & revents)
        fail("got invalid event");
    if (connection->ssl)
        buf_cnt = mbedtls_ssl_read(connection->ssl, buffer_wpos(buf), buffer_wlen(buf));
    else
        buf_cnt = mbedtls_net_recv(&connection->socket, buffer_wpos(buf), buffer_wlen(buf));
    if (buf_cnt <= 0) {
        ldap_connection_close(connection);
        if (buf_cnt < 0)
            mbedtls_fail("mbedtls_net_recv", buf_cnt);
        return;
    }
    buffer_fill(buf, buf_cnt);
    ldap_connection_respond(connection);
}

void write_cb(ev_loop *loop, ev_io *watcher, int revents)
{
    assert(revents == EV_WRITE);
    ldap_connection *connection = watcher->data;
    buffer_t *buf = &connection->send_buf;
    ssize_t buf_cnt;
    assert(connection->server->loop == loop);
    assert(&connection->write_watcher == watcher);

    if (connection->ssl)
        buf_cnt = mbedtls_ssl_write(connection->ssl, buffer_rpos(buf), buffer_rlen(buf));
    else
        buf_cnt = mbedtls_net_send(&connection->socket, buffer_rpos(buf), buffer_rlen(buf));
    if (buf_cnt < 0) {
        ldap_connection_close(connection);
        mbedtls_fail("mbedtls_net_send", buf_cnt);
    }
    buffer_toss(buf, buf_cnt);
    ldap_connection_respond(connection);
}

void handshake_cb(ev_loop *loop, ev_io *watcher, int revents)
{
    ldap_connection *connection = watcher->data;
    ldap_server *server = connection->server;
    assert(server->loop == loop);
    assert(&connection->write_watcher == watcher || &connection->read_watcher == watcher);
    int err;

    /* Send all outstanding requests and data using write_cb() first. */
    if (connection->request || !buffer_empty(&connection->send_buf))
        return write_cb(loop, watcher, revents);
    /* Create a new ssl context if needed. */
    if (!connection->ssl)
        connection->ssl = mbedtls_ssl_connection_new(server->ssl, &connection->socket);
    if ((err = mbedtls_ssl_handshake(connection->ssl))) {
        if (err == MBEDTLS_ERR_SSL_WANT_READ) {
            return ev_io_stop(loop, &connection->write_watcher);
        } else if (err == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return ev_io_start(loop, &connection->write_watcher);
        } else {
            /* The handshake failed, free the ssl context. */
            mbedtls_ssl_connection_free(connection->ssl);
            connection->ssl = NULL;
        }
    }
    /* Handshake over, set read/write watcher callbacks back. */
    ev_set_cb(&connection->read_watcher, read_cb);
    ev_set_cb(&connection->write_watcher, write_cb);
}

void goodbye_cb(ev_loop *loop, ev_io *watcher, int revents)
{
    ldap_connection *connection = watcher->data;
    assert(connection->server->loop == loop);
    assert(&connection->write_watcher == watcher || &connection->read_watcher == watcher);
    int err;

    /* Send all outstanding requests and data using write_cb() first. */
    if (connection->request || !buffer_empty(&connection->send_buf))
        return write_cb(loop, watcher, revents);
    if (connection->ssl && (err = mbedtls_ssl_close_notify(connection->ssl))) {
        if (err == MBEDTLS_ERR_SSL_WANT_READ) {
            return ev_io_stop(loop, &connection->write_watcher);
        } else if (err == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return ev_io_start(loop, &connection->write_watcher);
        } else {
            /* The goodbye failed. */
        }
    }
    /* Goodbye over, free connection. */
    ldap_connection_free(connection);
}

void delay_cb(ev_loop *loop, ev_timer *watcher, int revents)
{
    assert(revents == EV_TIMER);
    ldap_connection *connection = watcher->data;
    assert(connection->server->loop == loop);
    assert(&connection->delay_watcher == watcher);

    connection->delay = 0.0;
    ldap_connection_respond(connection);
}

/* Allocate and initialize a bare ldap_request from a request message. */
ldap_request *ldap_request_new(ldap_connection *connection, LDAPMessage_t *msg)
{
    assert(connection);
    assert(msg);
    ldap_request *request = XNEW0(ldap_request, 1);

    request->connection = connection;
    request->message = msg;
    request->reply = NULL;
    request->count = 0;
    /* Add the request to the connection's circular dlist. */
    ldap_request_add(&connection->request, request);
    lrinfo(request, "new request");
    return request;
}

/* Destroy and free an ldap_response. */
void ldap_request_free(ldap_request *request)
{
    if (request) {
        lrinfo(request, "completed");
        /* Remove the request from the connection's circular dlist. */
        ldap_request_rem(&request->connection->request, request);
        LDAPMessage_free(request->message);
        while (request->reply)
            ldap_reply_free(request->reply);
        free(request);
    }
}

/* Allocate and initialize a bind ldap_request from a bind message. */
ldap_request *ldap_request_bind(ldap_connection *connection, LDAPMessage_t *msg)
{
    assert(msg->protocolOp.present == LDAPMessage__protocolOp_PR_bindRequest);
    ldap_request *request = ldap_request_new(connection, msg);

    ldap_request_bind_pam(request);
    return request;
}

/* Allocate and initialize a search ldap_request from a search message. */
ldap_request *ldap_request_search(ldap_connection *connection, LDAPMessage_t *msg)
{
    assert(msg->protocolOp.present == LDAPMessage__protocolOp_PR_searchRequest);
    ldap_request *request = ldap_request_new(connection, msg);

    ldap_request_search_nss(request);
    return request;
}

/* Allocate and initialize an ldap_request from a extendedRequest message. */
ldap_request *ldap_request_extended(ldap_connection *connection, LDAPMessage_t *msg)
{
    assert(connection);
    assert(msg);
    assert(msg->protocolOp.present == LDAPMessage__protocolOp_PR_extendedReq);
    ldap_server *server = connection->server;
    ldap_request *request = ldap_request_new(connection, msg);
    ldap_reply *reply = ldap_reply_new(request);
    const ExtendedRequest_t *req = &msg->protocolOp.choice.extendedReq;
    ExtendedResponse_t *res = &reply->message.protocolOp.choice.extendedResp;

    reply->message.protocolOp.present = LDAPMessage__protocolOp_PR_extendedResp;
    LDAPString_set(&res->matchedDN, server->basedn);
    if (!strcmp((char *)req->requestName.buf, LDAPOID_StartTLS)) {
        lrinfo(request, "startTLS extended request");
        res->responseName = LDAPString_new(LDAPOID_StartTLS);
        if (server->ssl) {
            res->resultCode = ExtendedResponse__resultCode_success;
            LDAPString_set(&res->diagnosticMessage, "Starting TLS handshake...");
            /* Change the watcher callbacks for handshake. */
            ev_set_cb(&connection->read_watcher, handshake_cb);
            ev_set_cb(&connection->write_watcher, handshake_cb);
        } else {
            res->resultCode = ExtendedResponse__resultCode_protocolError;
            LDAPString_set(&res->diagnosticMessage, "TLS not enabled.");
        }
    } else {
        lrinfo(request, "unknown extended request %s", req->requestName.buf);
        res->resultCode = ExtendedResponse__resultCode_protocolError;
        LDAPString_set(&res->diagnosticMessage, "Unknown extended operation.");
    }
    return request;
}

/* Find and abandon a request from the circular dlist. */
void ldap_request_abandon(ldap_connection *connection, LDAPMessage_t *msg)
{
    assert(connection);
    assert(msg);
    MessageID_t msgid = msg->messageID;

    lcinfo(connection, "%ld:%s abandon request", msg->messageID, LDAPMessage_name(msg));
    /* Consume the message like we do for other request types. */
    LDAPMessage_free(msg);
    for (ldap_request r = connection->request; r; r = ldap_request_next(&connection->request, r))
        if (r->message->messageID == msgid)
            return ldap_request_free(r);
}

/* Process a single reply for an ldap_response. */
ldap_status_t ldap_request_respond(ldap_request *request)
{
    assert(request);
    assert(request->reply);
    ldap_status_t status = ldap_reply_respond(request->reply);

    /* If we sent a reply, rotate the connection to the next request. */
    if (status == RC_OK)
        request->connection->request = request->next;
    /* If we have no more replies, we are done. */
    if (!request->reply)
        ldap_request_free(request);
    return status;
}

/* Allocate and initialize a bare ldap_reply for an ldap_request. */
ldap_reply *ldap_reply_new(ldap_request *request)
{
    assert(request);
    ldap_reply *reply = XNEW0(ldap_reply, 1);

    reply->request = request;
    reply->message.messageID = request->message->messageID;
    /* Add the reply to the request's circular dlist. */
    ldap_reply_add(&request->reply, reply);
    request->count++;
    return reply;
}

/* Destroy and free an ldap_response. */
void ldap_reply_free(ldap_reply *reply)
{
    if (reply) {
        /* Remove the reply from the request's circular dlist. */
        ldap_reply_rem(&reply->request->reply, reply);
        LDAPMessage_done(&reply->message);
        free(reply);
    }
}

ldap_status_t ldap_reply_respond(ldap_reply *reply)
{
    assert(reply);
    ldap_request *request = reply->request;
    ldap_connection *connection = request->connection;
    ldap_status_t status = ldap_connection_send(connection, &reply->message);

    /* If the message was sent, we are done. */
    if (status == RC_OK) {
        lrdebug(request, "%s reply sent", LDAPMessage_name(&reply->message));
        ldap_reply_free(reply);
    }
    return status;
}
