#pragma once

// HTTP Basic authentication for every route and the /ws upgrade; on with --auth user:password.

#include "server/web/http/http_types.h"

// True when auth is off, or the request carries the right
// "Authorization: Basic base64(user:password)" header.
bool httpAuthorized(const HttpRequest &request);

// 401 with a JSON error body ({"ok": false, "error": {"code": "unauthorized"}})
// and a WWW-Authenticate challenge.
HttpResponse unauthorizedResponse();
