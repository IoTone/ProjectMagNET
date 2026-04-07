#ifndef CLAW_MDNS_H
#define CLAW_MDNS_H

#ifdef __cplusplus
extern "C" {
#endif

// Start mDNS with hostname and HTTP service advertisement.
void claw_mdns_start(const char *hostname, const char *instance_name);

#ifdef __cplusplus
}
#endif
#endif
