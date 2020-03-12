#include "stir_shaken.h"
#include "mongoose.h"


typedef void handler_t(struct mg_connection *nc, int event, void *hm, void *d);

typedef struct event_handler_s {
	char *uri;
	handler_t *f;
	int accepts_params;
} event_handler_t;

#define HANDLERS_N 10
event_handler_t event_handlers[HANDLERS_N];

static stir_shaken_status_t register_uri_handler(const char *uri, void *handler, int accepts_params)
{
	int i = 0;
	event_handler_t *e = NULL;

	while (i < HANDLERS_N) {

		e = &event_handlers[i];
		if (stir_shaken_zstr(e->uri)) {
			e->uri = strdup(uri);
			e->f = handler;
			e->accepts_params = accepts_params;
			return STIR_SHAKEN_STATUS_OK;
		}
		++i;
	}

	return STIR_SHAKEN_STATUS_FALSE;
}

static event_handler_t* handler_registered(struct mg_str *uri)
{
	int i = 0;
	event_handler_t *e = NULL;

	while (i < HANDLERS_N) {

		e = &event_handlers[i];
		if (e->uri) {
			if (e->accepts_params) {
				if (strstr(uri->p, e->uri)) {
					return e;
				}
			} else {
				if (!mg_vcmp(uri, e->uri)) {
					return e;
				}
			}
		}
		++i;
	}

	return NULL;
}

static void unregister_handlers(void)
{
	int i = 0;
	event_handler_t *e = NULL;

	while (i < HANDLERS_N) {

		e = &event_handlers[i];
		if (e->uri) {
			free(e->uri);
			e->uri = NULL;
		}
		++i;
	}
}

static void close_http_connection_with_error(struct mg_connection *nc, struct mbuf *io, const char *error_desc)
{
	if (nc) {
		if (stir_shaken_zstr(error_desc)) {
			mg_printf(nc, "HTTP/1.1 %s Not Found\r\n\r\n", STIR_SHAKEN_HTTP_REQ_404_NOT_FOUND);
		} else {
			char error_phrase[STIR_SHAKEN_BUFLEN] = { 0 };
			stir_shaken_error_desc_to_http_error_phrase(error_desc, error_phrase, STIR_SHAKEN_BUFLEN);
			mg_printf(nc, "HTTP/1.1 %s %s\r\n\r\n", STIR_SHAKEN_HTTP_REQ_404_NOT_FOUND, error_phrase);
		}
	}
	if (nc) mg_send_http_chunk(nc, "", 0);
	if (io) mbuf_remove(io, io->len);
	if (nc) nc->flags |= MG_F_SEND_AND_CLOSE;
}

static void close_http_connection(struct mg_connection *nc, struct mbuf *io)
{
	if (nc) mg_send_http_chunk(nc, "", 0);
	if (io) mbuf_remove(io, io->len);
	if (nc) nc->flags |= MG_F_SEND_AND_CLOSE;
}

static stir_shaken_ca_session_t* stir_shaken_ca_session_create(size_t sp_code, char *authorization_challenge)
{
	stir_shaken_ca_session_t *session = malloc(sizeof(stir_shaken_ca_session_t));

	if (!session)
		return NULL;

	memset(session, 0, sizeof(*session));
	session->spc = sp_code;
	session->authorization_challenge = strdup(authorization_challenge);
	session->state = STI_CA_SESSION_STATE_CHALLENGE;
	return session;
}

static void stir_shaken_ca_session_dctor(void *o)
{
	stir_shaken_ca_session_t *session = (stir_shaken_ca_session_t *) o;
	if (!session) return;
	if (session->authorization_challenge)
		free(session->authorization_challenge);
	return;
}

static void ca_handle_bad_request(struct mg_connection *nc, int event, void *hm, void *d)
{
	fprintif(1, "\n=== Handling: bad request...\n");
	return;
}

static void ca_handle_api_account(struct mg_connection *nc, int event, void *hm, void *d)
{
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "\n=== Handling: %s...\n", STI_CA_ACME_NEW_ACCOUNT_URL);
	return;
}

/*
 * Data posted by SP should be JWT of the form:
 *
 * {
 *	"protected": base64url({
 *		"alg": "ES256",
 *		"kid": " https://sti-ca.com/acme/acct/1",
 *		"nonce": "5XJ1L3lEkMG7tR6pA00clA",
 *		"url": " https://sti-ca.com/acme/new-order"
 *		})
 *	"payload": base64url({
 *		"csr": "5jNudRx6Ye4HzKEqT5...FS6aKdZeGsysoCo4H9P",
 *		"notBefore": "2016-01-01T00:00:00Z",
 *		"notAfter": "2016-01-08T00:00:00Z"
 *		}),
 *	"signature": "H6ZXtGjTZyUnPeKn...wEA4TklBdh3e454g"
 * }
*/
static void ca_handle_api_cert(struct mg_connection *nc, int event, void *hm, void *d)
{
	struct http_message *m = (struct http_message*) hm;
	struct mbuf *io = NULL;
	stir_shaken_context_t ss = { 0 };
	stir_shaken_ca_t *ca = (stir_shaken_ca_t*) d;
	cJSON *json = NULL;
	jwt_t *jwt = NULL;

	const char *spc = NULL;
	char *pCh = NULL;
	unsigned long long  sp_code;
	const char *csr_b64 = NULL;
	char csr[STIR_SHAKEN_BUFLEN] = { 0 };
	int csr_len = 0;
	char *token = NULL;
	char authz_url[STIR_SHAKEN_BUFLEN] = { 0 };
	char *expires = "2029-03-01T14:09:00Z";
	char *nb = "2019-01-01T00:00:00Z";
	char *na = "2029-01-01T00:00:00Z";
	char *authorization_challenge = NULL;

	char *nonce = "MYAuvOpaoIiywTezizk5vw";
	X509_REQ *req = NULL;
	stir_shaken_error_t error = STIR_SHAKEN_ERROR_GENERAL;
	const char *error_desc = NULL;
	stir_shaken_hash_entry_t *e = NULL;
	stir_shaken_ca_session_t *session = NULL;


	if (!m || !nc || !ca) {
		stir_shaken_set_error(&ca->ss, "Bad params, missing HTTP message, connection, and/or ca", STIR_SHAKEN_ERROR_ACME);
		goto fail;
	}

	io = &nc->recv_mbuf;

	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "\n=== Handling:\n%s\n", STI_CA_ACME_CERT_REQ_URL);
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "\n=== Message Body:\n%s\n", m->body.p);
	
	
	switch (event) {

		case MG_EV_HTTP_REQUEST:

			{
				if ((EINVAL == jwt_decode(&jwt, m->body.p, NULL, 0)) || !jwt) {
					stir_shaken_set_error(&ca->ss, "Cannot parse message body into JWT", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}

				csr_b64 = jwt_get_grant(jwt, "csr");
				if (stir_shaken_zstr(csr_b64)) {
					stir_shaken_set_error(&ca->ss, "JWT posted by SP is missing CSR", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}
				
				// Read SPC from JWT (ATIS standard doesn't reqire SPC to be set on JWT)
				spc = jwt_get_grant(jwt, "spc");
				if (stir_shaken_zstr(spc)) {
					stir_shaken_set_error(&ca->ss, "Cert request SPC (TNAuthList extension missing?)", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}

				sp_code = strtoul(spc, &pCh, 10); 
				if (sp_code > 0x10000 - 1) { 
					stir_shaken_set_error(&ca->ss, "SPC number too big", STIR_SHAKEN_ERROR_ACME_SPC_TOO_BIG);
					goto fail; 
				}

				if (*pCh != '\0') { 
					stir_shaken_set_error(&ca->ss, "SPC invalid", STIR_SHAKEN_ERROR_ACME_SPC_INVALID);
					goto fail; 
				}

				if (stir_shaken_b64_decode(csr_b64, csr, sizeof(csr)) < 1 || stir_shaken_zstr(csr)) {
					stir_shaken_set_error(&ca->ss, "Cannot decode CSR from base 64", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> CSR is:\n%s\n", csr);
				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> SPC is: %s\n", spc);

				req = stir_shaken_load_x509_req_from_pem(&ca->ss, csr);
				if (!req) {
					stir_shaken_set_error(&ca->ss, "Cannot load CSR into SSL", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}

				e = stir_shaken_hash_entry_find(ca->sessions, STI_CA_SESSIONS_MAX, sp_code);
				if (e) {
					stir_shaken_set_error(&ca->ss, "Authorization already in progress", STIR_SHAKEN_ERROR_ACME_SESSION_EXISTS);
					goto fail;
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "\t-> Requesting authorization...\n");

				// TODO generate 'expires'
				// TODO generate 'nb'
				// TODO generate 'na'
				// TODO generate Replay-Nonce
				snprintf(authz_url, STIR_SHAKEN_BUFLEN, "http://%s%s/%s", STI_CA_ACME_ADDR, STI_CA_ACME_AUTHZ_URL, spc);
				authorization_challenge = stir_shaken_acme_generate_auth_challenge(&ca->ss, "pending", expires, csr, nb, na, authz_url);
				if (stir_shaken_zstr(authorization_challenge)) {
					stir_shaken_set_error(&ca->ss, "Failed to create authorization challenge", STIR_SHAKEN_ERROR_ACME);
					goto fail;
				}

				// TODO queue challenge task/job
				session = stir_shaken_ca_session_create(sp_code, authorization_challenge);
				if (!session) {
					stir_shaken_set_error(&ca->ss, "Cannot create authorization session", STIR_SHAKEN_ERROR_ACME_SESSION_CREATE);
					goto fail;
				}

				e = stir_shaken_hash_entry_add(ca->sessions, STI_CA_SESSIONS_MAX, sp_code, session, stir_shaken_ca_session_dctor);
				if (!e) {
					stir_shaken_set_error(&ca->ss, "Oops. Failed to queue new session", STIR_SHAKEN_ERROR_ACME_SESSION_ENQUEUE);
					goto fail;
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "\t-> Added authorization session to queue\n");
				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "\t-> Sending authorization challenge:\n%s\n", authorization_challenge);

				mg_printf(nc, "HTTP/1.1 201 Created\r\nReplay-Nonce: %s\r\nLocation: %s\r\nContent-Length: %d\r\nContent-Type: application/json\r\n\r\n%s\r\n\r\n", nonce, authz_url, strlen(authorization_challenge), authorization_challenge);
				close_http_connection(nc, io);
				fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "=== OK\n");
			}

			break;
		
		case MG_EV_RECV:
			break;

		default:
			break;
	}

	if (json) {
		cJSON_Delete(json);
		json = NULL;
	}

	if (jwt) {
		jwt_free(jwt);
		jwt = NULL;
	}

	if (authorization_challenge) {
		free(authorization_challenge);
		authorization_challenge = NULL;
	}

	if (req) {
		X509_REQ_free(req);
		req = NULL;
	}

	stir_shaken_clear_error(&ca->ss);
	return;

fail:

	if (ca && stir_shaken_is_error_set(&ca->ss)) {
		error_desc = stir_shaken_get_error(&ca->ss, &error);
	}
	close_http_connection_with_error(nc, io, error_desc);

	if (json) {
		cJSON_Delete(json);
		json = NULL;
	}

	if (authorization_challenge) {
		free(authorization_challenge);
		authorization_challenge = NULL;
	}
	
	if (req) {
		X509_REQ_free(req);
		req = NULL;
	}

	stir_shaken_set_error(&ca->ss, "API CERT request failed", STIR_SHAKEN_ERROR_ACME);
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "=== FAIL\n");
	return;
}

static void ca_handle_api_authz(struct mg_connection *nc, int event, void *hm, void *d)
{
	struct http_message *m = (struct http_message*) hm;
	stir_shaken_ca_t *ca = (stir_shaken_ca_t*) d;
	struct mg_str authz_api_url = mg_mk_str(STI_CA_ACME_AUTHZ_URL);
	struct mbuf *io = NULL;
	
	char spc[STIR_SHAKEN_BUFLEN] = { 0 };;
	char *pCh = NULL;
	unsigned long long  sp_code;
	char *token = NULL;
	char authz_url[STIR_SHAKEN_BUFLEN] = { 0 };
	char *authz_challenge_details = NULL;
	const char *error_desc = NULL;
	stir_shaken_error_t error = STIR_SHAKEN_ERROR_GENERAL;
	int authz_attempt = 0;
	
	stir_shaken_hash_entry_t *e = NULL;
	stir_shaken_ca_session_t *session = NULL;
	
	
	if (!m || !nc || !ca) {
		stir_shaken_set_error(&ca->ss, "Bad params, missing HTTP message, connection, and/or ca", STIR_SHAKEN_ERROR_ACME);
		goto fail;
	}

	io = &nc->recv_mbuf;
	
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "\n=== Handling: %s...\n", STI_CA_ACME_AUTHZ_URL);
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "\n=== Message Body:\n%s\n", m->body.p);
	
	switch (event) {

		case MG_EV_HTTP_REQUEST:

			{
				if ((STIR_SHAKEN_STATUS_OK != stir_shaken_acme_authz_uri_to_spc(&ca->ss, m->uri.p, authz_api_url.p, spc, STIR_SHAKEN_BUFLEN)) || stir_shaken_zstr(spc)) {
					stir_shaken_set_error(&ca->ss, "Bad AUTHZ request, SPC is missing", STIR_SHAKEN_ERROR_ACME_AUTHZ_SPC);
					goto fail;
				}
				
				sp_code = strtoul(spc, &pCh, 10); 
				if (sp_code > 0x10000 - 1) { 
					stir_shaken_set_error(&ca->ss, "SPC number too big", STIR_SHAKEN_ERROR_ACME_SPC_TOO_BIG);
					goto fail; 
				}

				if (*pCh != '\0') { 
					stir_shaken_set_error(&ca->ss, "SPC invalid", STIR_SHAKEN_ERROR_ACME_SPC_INVALID);
					goto fail; 
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> SPC is: %s\n", spc);

				// TODO check if challenge task/job exists for this SPC
				e = stir_shaken_hash_entry_find(ca->sessions, STI_CA_SESSIONS_MAX, sp_code);
				if (!e) {
					stir_shaken_set_error(&ca->ss, "Authorization session for this SPC does not exist", STIR_SHAKEN_ERROR_ACME_SESSION_NOTFOUND);
					goto fail;
				}

				session = e->data;
				if (!session) {
					stir_shaken_set_error(&ca->ss, "Oops. Authorization session not set", STIR_SHAKEN_ERROR_ACME_SESSION_NOT_SET);
					goto fail;
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> Authorization session in progress\n");
				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> Authorization session challenge was:\n%s\n", session->authorization_challenge);

				// TODO generate random token
				token = "DGyRejmCefe7v4NfDGDKfA";

				// TODO process authz attempts
				authz_attempt = 0;
				snprintf(authz_url, STIR_SHAKEN_BUFLEN, "http://%s%s/%s/%d", STI_CA_ACME_ADDR, STI_CA_ACME_AUTHZ_URL, spc, authz_attempt);

				authz_challenge_details = stir_shaken_acme_generate_auth_challenge_details(&ca->ss, spc, token, authz_url);
				if (stir_shaken_zstr(authz_challenge_details)) {
					stir_shaken_set_error(&ca->ss, "AUTHZ request failed, could not produce challenge details", STIR_SHAKEN_ERROR_ACME_AUTHZ_DETAILS);
					goto fail;
				}

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "-> Sending challenge details:\n%s\n", authz_challenge_details);

				mg_printf(nc, "HTTP/1.1 200 You are more than welcome. Here is your challenge:\r\nContent-Length: %d\r\nContent-Type: application/json\r\n\r\n%s\r\n\r\n", strlen(authz_challenge_details), authz_challenge_details);
				close_http_connection(nc, io);
				fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "=== OK\n");
			}

			break;

		case MG_EV_RECV:
			break;

		default:
			break;
	}

	if (authz_challenge_details) {
		free(authz_challenge_details);
		authz_challenge_details = NULL;
	}

	return;

fail:

	if (ca && stir_shaken_is_error_set(&ca->ss)) {
		error_desc = stir_shaken_get_error(&ca->ss, &error);
	}
	close_http_connection_with_error(nc, io, error_desc);

	if (authz_challenge_details) {
		free(authz_challenge_details);
		authz_challenge_details = NULL;
	}
	
	stir_shaken_set_error(&ca->ss, "API AUTHZ request failed", STIR_SHAKEN_ERROR_ACME);
	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "=== FAIL\n");
	return;
}

static void ca_event_handler(struct mg_connection *nc, int event, void *hm, void *d)
{
	struct http_message *m = (struct http_message*) hm;
	struct mbuf *io = NULL;
	event_handler_t *evh = NULL;
	stir_shaken_ca_t *ca = (stir_shaken_ca_t*) d;
	stir_shaken_error_t error = STIR_SHAKEN_ERROR_GENERAL;
	const char *error_desc = NULL;
	char err_buf[STIR_SHAKEN_ERROR_BUF_LEN] = { 0 };
	struct mg_str api_url = mg_mk_str(STI_CA_ACME_API_URL);

	
	if (!ca) {
		stir_shaken_set_error(&ca->ss, "Bad params", STIR_SHAKEN_ERROR_ACME);
		goto exit;
	}

	fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "Event [%d]...\n", event);

	if (nc) {
		io = &nc->recv_mbuf;
	}
	
	switch (event) {

		case MG_EV_ACCEPT:
			{
				char addr[32];
				mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "%p: Connection from %s\r\n", nc, addr);
				break;
			}

		case MG_EV_RECV:
			fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "RECV... \r\n");
			break;

		case MG_EV_HTTP_REQUEST:
			{
				unsigned int port_i = 0;

				fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "Processing HTTP request...\n");

				if (m->uri.p) {

					if (!mg_strstr(m->uri, api_url)) {
						
						if (ca && stir_shaken_is_error_set(&ca->ss)) {
							error_desc = stir_shaken_get_error(&ca->ss, &error);
						}

						close_http_connection_with_error(nc, io, error_desc);
						stir_shaken_set_error(&ca->ss, "URL is not handled by ACME API. Closed HTTP connection", STIR_SHAKEN_ERROR_ACME);
						break;
					}

					fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "Searching handler for %s...\n", m->uri.p);

					evh = handler_registered(&m->uri);

					if (!evh) {
						
						if (ca && stir_shaken_is_error_set(&ca->ss)) {
							error_desc = stir_shaken_get_error(&ca->ss, &error);
						}

						close_http_connection_with_error(nc, io, error_desc);
						stir_shaken_set_error(&ca->ss, "Handler not found. Closed HTTP connection", STIR_SHAKEN_ERROR_ACME);

						break;
					}

					fprintif(STIR_SHAKEN_LOGLEVEL_MEDIUM, "\nHandler found\n");
					evh->f(nc, event, hm, d);
				}
				break;
			}

		default:
			break;
	}

exit:
	if (stir_shaken_is_error_set(&ca->ss)) {
		error_desc = stir_shaken_get_error(&ca->ss, &error);
		fprintif(STIR_SHAKEN_LOGLEVEL_BASIC, "Error. %s\n", error_desc);
		stir_shaken_clear_error(&ca->ss);
	}
	return;
}

stir_shaken_status_t stir_shaken_run_ca_service(stir_shaken_context_t *ss, stir_shaken_ca_t *ca)
{
	struct mg_mgr mgr = { 0 };
	struct mg_connection *nc = NULL;
	char port[100] = { 0 };
	struct mg_bind_opts bopts = { 0 };
	struct mg_http_endpoint_opts opts = { 0 };

	
	if (!ca)
		return STIR_SHAKEN_STATUS_TERM;
	
	memset(&opts, 0, sizeof(opts));
	opts.user_data = ca;
	bopts.user_data = ca;

	mg_mgr_init(&mgr, NULL);
	
	if (ca->port == 0)
		ca->port = STIR_SHAKEN_DEFAULT_CA_PORT;

	snprintf(port, 100, ":%u", ca->port); 
	nc = mg_bind_opt(&mgr, port, ca_event_handler, ca, bopts);
	if (!nc) {
		stir_shaken_set_error(ss, "Cannnot bind to port", STIR_SHAKEN_ERROR_BIND);
		return STIR_SHAKEN_STATUS_FALSE;
	}

	register_uri_handler(STI_CA_ACME_NEW_ACCOUNT_URL, ca_handle_api_account, 0);
	register_uri_handler(STI_CA_ACME_CERT_REQ_URL, ca_handle_api_cert, 0);
	register_uri_handler(STI_CA_ACME_AUTHZ_URL, ca_handle_api_authz, 1);

	mg_set_protocol_http_websocket(nc);

	for (;;) {
		mg_mgr_poll(&mgr, 10000);
	}

	unregister_handlers();
	mg_mgr_free(&mgr);

	return STIR_SHAKEN_STATUS_OK;
}
