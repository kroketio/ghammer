#pragma once

class HttpServer;

// HTTP side of API v2: bytes only. JSON control traffic is on the WebSocket (web/realtime_hub.h).
//   POST /api/2/files                        {kind: thumb|editor, files: [...]} -> file pack
//   GET  /api/2/source/<pack>/<name>/<type>  full-resolution source map
//   PUT  /api/2/source/<pack>/<name>/<type>  replace a map (user packs only)
//   PUT  /api/2/lut/<pack>/<name>            create / replace a colour LUT strip (user "lut" packs only);
//                                            header X-Lut-Grade: base64 JSON of its adjustment stack (optional)
namespace RouteV2 {
void install(HttpServer *server);
}
