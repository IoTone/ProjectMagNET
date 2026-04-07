#ifndef CRAW_MDNS_H
#define CRAW_MDNS_H

#ifdef __cplusplus
extern "C" {
#endif

// Start mDNS with hostname and HTTP service advertisement.
void craw_mdns_start(const char *hostname, const char *instance_name);

#ifdef __cplusplus
}
#endif
#endif
