#pragma once

#include "MaiFirstPartyRoute.h"

#include <curl/curl.h>
#include <string>

namespace maiFirstPartyRoute {

// Match the complete authority, not a suffix or an attacker-controlled prefix.
// Signed OSS URLs and test hosts must never inherit the account service route.
inline bool eligible(const std::string& url, bool controlRequest) {
    constexpr char origin[] = "https://ichat.life/";
    return controlRequest && url.compare(0, sizeof(origin) - 1, origin) == 0;
}

inline CURLcode apply(CURL* curl, int route) {
    if (route == MAI_FIRST_PARTY_ROUTE_SYSTEM) return CURLE_OK;
    const char* doh = route == MAI_FIRST_PARTY_ROUTE_ALI_DOH ? "https://223.5.5.5/dns-query"
        : route == MAI_FIRST_PARTY_ROUTE_CLOUDFLARE_DOH ? "https://1.1.1.1/dns-query" : nullptr;
    if (!doh) return CURLE_COULDNT_RESOLVE_HOST;
    // Keep the original request URL, SNI and hostname verification intact.
    CURLcode code = curl_easy_setopt(curl, CURLOPT_PROXY, "");
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_DOH_URL, doh);
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_DOH_SSL_VERIFYPEER, 1L);
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_DOH_SSL_VERIFYHOST, 2L);
    return code;
}

}  // namespace maiFirstPartyRoute
