#pragma once

// The host chooses a verified route before the first-party request is sent.
// Zero preserves the system proxy; positive values select a direct DoH route.
// A negative value rejects the request without sending account credentials.
typedef int (*MaiFirstPartyRouteSelector)(const char* url_utf8,
                                         const char* ca_path_utf8,
                                         void* context);
typedef void (*MaiFirstPartyRouteInvalidator)(void* context);

enum MaiFirstPartyRoute {
    MAI_FIRST_PARTY_ROUTE_UNAVAILABLE = -1,
    MAI_FIRST_PARTY_ROUTE_SYSTEM = 0,
    MAI_FIRST_PARTY_ROUTE_ALI_DOH = 1,
    MAI_FIRST_PARTY_ROUTE_CLOUDFLARE_DOH = 2
};
