#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>

NetworkSnapshot platform_networks(void) {
    NetworkSnapshot snapshot = { .status = METRIC_UNAVAILABLE };
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) return snapshot;

    for (struct ifaddrs *address = addresses; address;
         address = address->ifa_next) {
        if (!address->ifa_name || !address->ifa_data || !address->ifa_addr ||
            address->ifa_addr->sa_family != AF_LINK) continue;
        if (snapshot.count == MAX_NETWORK_INTERFACES) { snapshot.truncated = true; continue; }

        struct if_data *data = (struct if_data *)address->ifa_data;
        NetworkInterface *network = &snapshot.items[snapshot.count++];
        snprintf(network->name, sizeof(network->name), "%s", address->ifa_name);
        network->up = (address->ifa_flags & IFF_UP) != 0;
        network->received = data->ifi_ibytes;
        network->transmitted = data->ifi_obytes;

    }
    freeifaddrs(addresses);
    snapshot.status = METRIC_OK;
    return snapshot;
}
