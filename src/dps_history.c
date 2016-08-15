#include <malloc.h>
#include <string.h>
#include <assert.h>
#include <uv.h>
#include <dps.h>
#include <search.h>
#include "dps_history.h"

#define HISTORY_THRESHOLD   10

/*
 * How long to keep publication history (in nanoseconds)
 */
#define PUB_HISTORY_LIFETIME   (30ul * 1000000000ul)

void DPS_FreshenHistory(DPS_History* history)
{
    if (history->count > HISTORY_THRESHOLD) {
        uint64_t now = uv_hrtime();
        DPS_PubHistory* ph = history->oldest;

        while (ph) {
            if (now < ph->expiration) {
                break;
            }
            history->oldest = ph->next;
            free(ph);
            --history->count;
            ph = history->oldest;
        }
        if (!ph) {
            history->newest = NULL;
        }
    }
}

DPS_Status DPS_AppendPubHistory(DPS_History* history, DPS_UUID* pubId, uint32_t serialNumber, DPS_NodeAddress* addr)
{
    uint64_t now = uv_hrtime();
    DPS_PubHistory* ph = calloc(1, sizeof(DPS_PubHistory));

    if (!ph) {
        return DPS_ERR_RESOURCES;
    }
    ph->pub.id = *pubId;
    ph->pub.sn = serialNumber;
    if (addr) {
        ph->pub.addr = *addr;
    }
    ph->expiration = now + PUB_HISTORY_LIFETIME;
    if (history->newest) {
        history->newest->next = ph;
        history->newest = ph;
    } else {
        assert(!history->oldest);
        assert(!history->count);
        history->newest = history->oldest = ph;
    }
    ++history->count;
    return DPS_OK;
}

int DPS_PublicationIsStale(DPS_History* history, DPS_UUID* pubId, uint32_t serialNumber)
{
    DPS_PubHistory* ph = history->oldest;
    /*
     * TODO - for scalabililty nees to replace the linear search with a more efficient lookup 
     */
    while (ph) {
        if ((ph->pub.sn == serialNumber) && (memcmp(&ph->pub.id, pubId, sizeof(pubId->val)) == 0)) {
            return DPS_TRUE;
        }
        ph = ph->next;
    }
    return DPS_FALSE;
}

void DPS_HistoryFree(DPS_History* history)
{
    DPS_PubHistory* ph = history->oldest;
    while (ph) {
        DPS_PubHistory* next = ph->next;
        free(ph);
        ph = next;
    }
    history->oldest = NULL;
    history->newest = NULL;
    history->count = 0;
}

DPS_Status DPS_LookupPublisher(DPS_History* history, const DPS_UUID* pubId, uint32_t serialNumber, DPS_NodeAddress** addr)
{
    DPS_PubHistory* ph = history->oldest;
    /*
     * TODO - for scalabililty need to replace the linear search with a more efficient lookup 
     */
    *addr = NULL;
    while (ph) {
        if ((ph->pub.sn == serialNumber) && (memcmp(&ph->pub.id, pubId, sizeof(pubId->val)) == 0)) {
            if (ph->pub.addr.inaddr.ss_family) {
                *addr = &ph->pub.addr;
            }
            return DPS_OK;
        }
        ph = ph->next;
    }
    return DPS_ERR_MISSING;
}

