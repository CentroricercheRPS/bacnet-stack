/*####COPYRIGHTBEGIN####
 -------------------------------------------
 Copyright (C) 2004 Steve Karg

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to:
 The Free Software Foundation, Inc.
 59 Temple Place - Suite 330
 Boston, MA  02111-1307, USA.

 As a special exception, if other files instantiate templates or
 use macros or inline functions from this file, or you compile
 this file and link it with other works to produce a work based
 on this file, this file does not by itself cause the resulting
 work to be covered by the GNU General Public License. However
 the source code for this file must still be made available in
 accordance with section (3) of the GNU General Public License.

 This exception does not invalidate any other reasons why a work
 based on this file might be covered by the GNU General Public
 License.
 -------------------------------------------
####COPYRIGHTEND####*/
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "bacnet/bits.h"
#include "bacnet/config.h"
#include "bacnet/bacaddr.h"
#include "bacnet/bacdef.h"
#include "bacnet/bacdcode.h"
#include "bacnet/readrange.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/binding/address.h"

/* we are likely compiling the demo command line tools if print enabled */
#if !defined(BACNET_ADDRESS_CACHE_FILE)
#if PRINT_ENABLED
#define BACNET_ADDRESS_CACHE_FILE
#endif
#endif

/** @file address.c  Handle address binding */

/* This module is used to handle the address binding that */
/* occurs in BACnet.  A device id is bound to a MAC address. */
/* The normal method is using Who-Is, and using the data from I-Am */

static uint32_t Top_Protected_Entry;
static uint32_t Own_Device_ID = 0xFFFFFFFF;

/* The address cache is used for binding to BACnet devices */
/* The number of entries corresponds to the number of */
/* devices that might respond to an I-Am on the network. */
/* If your device is a simple server and does not need to bind, */
/* then you don't need to use this. */
#if !defined(MAX_ADDRESS_CACHE)
#define MAX_ADDRESS_CACHE 255
#endif

static struct Address_Cache_Entry {
    uint8_t Flags;
    uint32_t device_id;
    unsigned max_apdu;
    BACNET_ADDRESS address;
    uint32_t TimeToLive;
} Address_Cache[MAX_ADDRESS_CACHE];

/* State flags for cache entries */

/* Address cache entry in use */
#define BAC_ADDR_IN_USE BIT(0)
/* Bind request outstanding for entry */
#define BAC_ADDR_BIND_REQ BIT(1)
/* Static address mapping - does not expire */
#define BAC_ADDR_STATIC BIT(2)
/* Opportunistically added address with short TTL */
#define BAC_ADDR_SHORT_TTL BIT(3)
/* Freed up but held for caller to fill */
#define BAC_ADDR_RESERVED BIT(7)

#define BAC_ADDR_SECS_1HOUR 3600 /* 60x60 */
#define BAC_ADDR_SECS_1DAY 86400 /* 60x60x24 */

#define BAC_ADDR_LONG_TIME BAC_ADDR_SECS_1DAY
#define BAC_ADDR_SHORT_TIME BAC_ADDR_SECS_1HOUR
#define BAC_ADDR_FOREVER 0xFFFFFFFF /* Permenant entry */

/**
 * @brief Set the index of the first (top) address being protected.
 *
 * @param top_protected_entry_index  top protected index [0..n-1]
 */
void address_protected_entry_index_set(uint32_t top_protected_entry_index)
{
    if (top_protected_entry_index <= (MAX_ADDRESS_CACHE - 1)) {
        Top_Protected_Entry = top_protected_entry_index;
    }
}

/**
 * @brief Set the address of our own device.
 *
 * @param own_id  Own device id
 */
void address_own_device_id_set(uint32_t own_id)
{
    Own_Device_ID = own_id;
}

/**
 * @brief Check if the given source and destination address can be matched
 *        by checking the length, net and MAC address.
 *
 * @param dest  Destination address
 * @param src   Source address
 *
 * @return true for a match, false otherwise
 */
bool address_match(BACNET_ADDRESS *dest, BACNET_ADDRESS *src)
{
    uint8_t i = 0;
    uint8_t max_len = 0;

    if (dest == src) {
        return (true);
    }
    if (dest->mac_len != src->mac_len) {
        return false;
    }
    max_len = dest->mac_len;
    if (max_len > MAX_MAC_LEN) {
        max_len = MAX_MAC_LEN;
    }
    for (i = 0; i < max_len; i++) {
        if (dest->mac[i] != src->mac[i]) {
            return false;
        }
    }
    if (dest->net != src->net) {
        return false;
    }

    /* if local, ignore remaining fields */
    if (dest->net == 0) {
        return true;
    }

    if (dest->len != src->len) {
        return false;
    }
    max_len = dest->len;
    if (max_len > MAX_MAC_LEN) {
        max_len = MAX_MAC_LEN;
    }
    for (i = 0; i < max_len; i++) {
        if (dest->adr[i] != src->adr[i]) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Remove a device from the address list.
 *
 * @param device_id  ID of the device
 */
void address_remove_device(uint32_t device_id)
{
    struct Address_Cache_Entry *pMatch;
    uint32_t index = 0;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            pMatch->Flags = 0;
            if (index < Top_Protected_Entry) {
                Top_Protected_Entry--;
            }
            break;
        }
    }

    return;
}

/**
 * @brief Search the cache for the entry nearest expiry and delete it. Mark the
 * entry as reserved with a 1 hour TTL and return a pointer to the reserved
 * entry. Will not delete a static entry and returns NULL pointer if no
 * entry available to free up. Does not check for free entries as it is
 * assumed we are calling this due to the lack of those.
 *
 * @return Pointer to the entry that has been removed or NULL.
 */
static struct Address_Cache_Entry *address_remove_oldest(void)
{
    struct Address_Cache_Entry *pMatch;
    struct Address_Cache_Entry *pCandidate;
    uint32_t ulTime;
    unsigned index;

    pCandidate = NULL;
    if (Top_Protected_Entry > (MAX_ADDRESS_CACHE - 1)) {
        return pCandidate;
    }
    /* Longest possible non static time to live */
    ulTime = BAC_ADDR_FOREVER - 1;

    /* First pass - try only in use and bound entries */

    for (index = Top_Protected_Entry; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags &
                (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ | BAC_ADDR_STATIC)) ==
            BAC_ADDR_IN_USE) {
            if (pMatch->TimeToLive <= ulTime) { 
                /* Shorter lived entry found */
                ulTime = pMatch->TimeToLive;
                pCandidate = pMatch;
            }
        }
    }

    if (pCandidate != NULL) { 
        /* Found something to free up */
        pCandidate->Flags = BAC_ADDR_RESERVED;
        /* only reserve it for a short while */
        pCandidate->TimeToLive = BAC_ADDR_SHORT_TIME; 
        return (pCandidate);
    }

    /* Second pass - try in use and un bound as last resort */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags &
                (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ | BAC_ADDR_STATIC)) ==
            ((uint8_t)(BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ))) {
            if (pMatch->TimeToLive <= ulTime) { /* Shorter lived entry found */
                ulTime = pMatch->TimeToLive;
                pCandidate = pMatch;
            }
        }
    }

    if (pCandidate != NULL) { 
        /* Found something to free up */
        pCandidate->Flags = BAC_ADDR_RESERVED;
        /* only reserve it for a short while */
        pCandidate->TimeToLive = BAC_ADDR_SHORT_TIME;
    }

    return (pCandidate);
}

/**
 * Initialize a BACNET_MAC_ADDRESS
 *
 * @param mac [out] BACNET_MAC_ADDRESS structure
 * @param adr [in] address to initialize, null if empty
 * @param len [in] length of address in bytes
 */
void address_mac_init(BACNET_MAC_ADDRESS *mac, uint8_t *adr, uint8_t len)
{
    uint8_t i = 0;

    if (mac) {
        if (adr && (len <= sizeof(mac->adr))) {
            for (i = 0; i < len; i++) {
                mac->adr[i] = adr[i];
            }
            mac->len = len;
        } else {
            mac->len = 0;
        }
    }
}

/** Parse an ASCII string for a bacnet-address
 *
 * @param mac [out] BACNET_MAC_ADDRESS structure to store the results
 * @param arg [in] nul terminated ASCII string to parse
 *
 * @return true if the address was parsed
 */
bool address_mac_from_ascii(BACNET_MAC_ADDRESS *mac, const char *arg)
{
    unsigned a[6] = { 0 }, p = 0;
    uint16_t port = 0;
    int c;
    bool status = false;

    if (!(mac && arg)) {
        return false;
    }
    c = sscanf(arg, "%3u.%3u.%3u.%3u:%5u", &a[0], &a[1], &a[2], &a[3], &p);
    if ((c == 4) || (c == 5)) {
        mac->adr[0] = a[0];
        mac->adr[1] = a[1];
        mac->adr[2] = a[2];
        mac->adr[3] = a[3];
        if (c == 4) {
            port = 0xBAC0U;
        } else {
            port = (uint16_t)p;
        }
        encode_unsigned16(&mac->adr[4], port);
        mac->len = 6;
        status = true;
    } else {
        c = sscanf(arg, "%2x:%2x:%2x:%2x:%2x:%2x", &a[0], &a[1], &a[2], &a[3],
            &a[4], &a[5]);
        if (c == 6) {
            mac->adr[0] = a[0];
            mac->adr[1] = a[1];
            mac->adr[2] = a[2];
            mac->adr[3] = a[3];
            mac->adr[4] = a[4];
            mac->adr[5] = a[5];
            mac->len = 6;
            status = true;
        } else if (c == 1) {
            if (a[0] <= 255) {
                mac->adr[0] = a[0];
                mac->len = 1;
                status = true;
            }
        }
    }

    return status;
}

#ifdef BACNET_ADDRESS_CACHE_FILE
/* File format:
DeviceID MAC SNET SADR MAX-APDU
4194303 05 0 0 50
55555 C0:A8:00:18:BA:C0 26001 19 50
note: useful for MS/TP Slave static binding
*/
static const char *Address_Cache_Filename = "address_cache";

static void address_file_init(const char *pFilename)
{
    FILE *pFile = NULL; /* stream pointer */
    char line[256] = { "" }; /* holds line from file */
    long device_id = 0;
    unsigned snet = 0;
    unsigned max_apdu = 0;
    char mac_string[80] = { "" }, sadr_string[80] = { "" };
    BACNET_ADDRESS src = { 0 };
    BACNET_MAC_ADDRESS mac = { 0 };
    int index = 0;

    pFile = fopen(pFilename, "r");
    if (pFile) {
        while (fgets(line, (int)sizeof(line), pFile) != NULL) {
            /* ignore comments */
            if (line[0] != ';') {
                if (sscanf(line, "%7ld %79s %5u %79s %4u", &device_id,
                        &mac_string[0], &snet, &sadr_string[0],
                        &max_apdu) == 5) {
                    if (address_mac_from_ascii(&mac, mac_string)) {
                        src.mac_len = mac.len;
                        for (index = 0; index < MAX_MAC_LEN; index++) {
                            src.mac[index] = mac.adr[index];
                        }
                    }
                    src.net = (uint16_t)snet;
                    if (snet) {
                        if (address_mac_from_ascii(&mac, sadr_string)) {
                            src.len = mac.len;
                            for (index = 0; index < MAX_MAC_LEN; index++) {
                                src.adr[index] = mac.adr[index];
                            }
                        }
                    } else {
                        src.len = 0;
                        for (index = 0; index < MAX_MAC_LEN; index++) {
                            src.adr[index] = 0;
                        }
                    }
                    address_add((uint32_t)device_id, max_apdu, &src);
                    /* Mark as static entry */
                    address_set_device_TTL((uint32_t)device_id, 0, true);
                }
            }
        }
        fclose(pFile);
    }

    return;
}
#endif

/**
 * Clear down the cache and make sure the full complement of entries are
 * available. Assume no persistance of memory.
 */
void address_init(void)
{
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    Top_Protected_Entry = 0;
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        pMatch->Flags = 0;
    }
#ifdef BACNET_ADDRESS_CACHE_FILE
    address_file_init(Address_Cache_Filename);
#endif
    return;
}

/**
 * Clear down the cache of any non bound, expired  or reserved entries.
 * Leave static and unexpired bound entries alone. For use where the cache
 * is held in persistant memory which can survive a reset or power cycle.
 * This reduces the network traffic on restarts as the cache will have much
 * of its entries intact.
 */
void address_init_partial(void)
{
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags & BAC_ADDR_IN_USE) != 0) { 
            /* It's in use so let's check further */
            if (((pMatch->Flags & BAC_ADDR_BIND_REQ) != 0) ||
                (pMatch->TimeToLive == 0)) {
                pMatch->Flags = 0;
            }
        }

        if ((pMatch->Flags & BAC_ADDR_RESERVED) != 0) { 
            /* Reserved entries should be cleared */
            pMatch->Flags = 0;
        }
    }
#ifdef BACNET_ADDRESS_CACHE_FILE
    address_file_init(Address_Cache_Filename);
#endif

    return;
}

/**
 * Set the TTL info for the given device entry. If it is a bound entry we
 * set it to static or normal and can change the TTL. If it is unbound we
 * can only set the TTL. This is done as a seperate function at the moment
 * to avoid breaking the current API.
 *
 * @param device_id  Device-Id
 * @param TimeOut  Timeout in seconds.
 * @param StaticFlag  Flag indicating if being a static address.
 */
void address_set_device_TTL(
    uint32_t device_id, uint32_t TimeOut, bool StaticFlag)
{
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            if ((pMatch->Flags & BAC_ADDR_BIND_REQ) == 0) { 
                /* If bound then we have either static or normaal */
                if (StaticFlag) {
                    pMatch->Flags |= BAC_ADDR_STATIC;
                    pMatch->TimeToLive = BAC_ADDR_FOREVER;
                } else {
                    pMatch->Flags &= ~BAC_ADDR_STATIC;
                    pMatch->TimeToLive = TimeOut;
                }
            } else {
            	/* For unbound we can only set the time to live */
                pMatch->TimeToLive = TimeOut; 
            }
            break; /* Exit now if found at all - bound or unbound */
        }
    }
}

/**
 * Return the cached address for the given device-id
 *
 * @param device_id  Device-Id
 * @param max_apdu  Pointer to a variable, taking the maximum APDU size.
 * @param src  Pointer to address structure for return.
 */
bool address_get_by_device(
    uint32_t device_id, unsigned *max_apdu, BACNET_ADDRESS *src)
{
    struct Address_Cache_Entry *pMatch;
    bool found = false; /* return value */
    unsigned index;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            if ((pMatch->Flags & BAC_ADDR_BIND_REQ) == 0) { 
                /* If bound then fetch data */
                bacnet_address_copy(src, &pMatch->address);
                *max_apdu = pMatch->max_apdu;
                /* Prove we found it */
                found = true; 
            }
            /* Exit now if found at all - bound or unbound */
            break; 
        }
    }

    return found;
}

/**
 * Find a device id from a given MAC address.
 *
 * @param src  Pointer to address structure to search for.
 * @param device_id  Pointer to the device id variable for return.
 *
 * @return true/false
 */
bool address_get_device_id(BACNET_ADDRESS *src, uint32_t *device_id)
{
    struct Address_Cache_Entry *pMatch;
    bool found = false; /* return value */
    unsigned index;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) == 
            BAC_ADDR_IN_USE) { 
            /* If bound */
            if (bacnet_address_same(&pMatch->address, src)) {
                if (device_id) {
                    *device_id = pMatch->device_id;
                }
                found = true;
                break;
            }
        }
    }

    return found;
}

/**
 * Add a device using the given id, max_apdu and address.
 *
 * @param device_id  Pointer to the device id variable for return.
 * @param max_apdu  Maximum APDU size.
 * @param src  Pointer to address structure to add.
 */
void address_add(uint32_t device_id, unsigned max_apdu, BACNET_ADDRESS *src)
{
    bool found = false; /* return value */
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    if (Own_Device_ID == device_id) {
        return;
    }

    /* Note: Previously this function would ignore bind request
       marked entries and in fact would probably overwrite the first
       bind request entry blindly with the device info which may
       have nothing to do with that bind request. Now it honours the
       bind request if it exists */

    /* existing device or bind request outstanding - update address */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        /* Device already in the list, then update the values. */
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            bacnet_address_copy(&pMatch->address, src);
            pMatch->max_apdu = max_apdu;
            /* Pick the right time to live */
            if ((pMatch->Flags & BAC_ADDR_BIND_REQ) != 0) { 
                /* Bind requested so long time */
                pMatch->TimeToLive = BAC_ADDR_LONG_TIME;
            } else if ((pMatch->Flags & BAC_ADDR_STATIC) != 0) { 
                /* Static already so make sure it never expires */
                pMatch->TimeToLive = BAC_ADDR_FOREVER;
            } else if ((pMatch->Flags & BAC_ADDR_SHORT_TTL) != 0) { 
                /* Opportunistic entry so leave on short fuse */
                pMatch->TimeToLive = BAC_ADDR_SHORT_TIME;
            } else {
            	/* Renewing existing entry */
                pMatch->TimeToLive = BAC_ADDR_LONG_TIME; 
            }
            /* Clear bind request flag just in case */
            pMatch->Flags &= ~BAC_ADDR_BIND_REQ; 
            found = true;
            break;
        }
    }
    /* New device - add to cache if there is room. */
    if (!found) {
        for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
            pMatch = &Address_Cache[index];
            if ((pMatch->Flags & BAC_ADDR_IN_USE) == 0) {
                pMatch->Flags = BAC_ADDR_IN_USE;
                pMatch->device_id = device_id;
                pMatch->max_apdu = max_apdu;
                bacnet_address_copy(&pMatch->address, src);
                /* Opportunistic entry so leave on short fuse */
                pMatch->TimeToLive = BAC_ADDR_SHORT_TIME; 
                found = true;
                break;
            }
        }
    }
    /* If adding has failed, see if we can squeeze it in by removed the oldest
     * entry. */
    if (!found) {
        pMatch = address_remove_oldest();
        if (pMatch != NULL) {
            pMatch->Flags = BAC_ADDR_IN_USE;
            pMatch->device_id = device_id;
            pMatch->max_apdu = max_apdu;
            bacnet_address_copy(&pMatch->address, src);
            /* Opportunistic entry so leave on short fuse */
            pMatch->TimeToLive = BAC_ADDR_SHORT_TIME; 
        }
    }
    return;
}

/**
 * Check if the device is in the list. If yes, return the values.
 * Otherwise add the device to the list.
 * Returns true if device is already bound.
 * Also returns the address and max apdu if already bound.
 *
 * @param device_id  Pointer to the device id variable for return.
 * @param device_ttl  Pointer to a variable taking the time to live in seconds.
 * @param max_apdu  Max APDU size of the device.
 * @param src  Pointer to the BACnet address.
 *
 * @return true if device is already bound
 */
bool address_device_bind_request(uint32_t device_id,
    uint32_t *device_ttl,
    unsigned *max_apdu,
    BACNET_ADDRESS *src)
{
    bool found = false; /* return value */
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    /* existing device - update address info if currently bound */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            if ((pMatch->Flags & BAC_ADDR_BIND_REQ) == 0) { 
                /* Already bound */
                found = true;
                if (src) {
                    bacnet_address_copy(src, &pMatch->address);
                }
                if (max_apdu) {
                    *max_apdu = pMatch->max_apdu;
                }
                if (device_ttl) {
                    *device_ttl = pMatch->TimeToLive;
                }
                if ((pMatch->Flags & BAC_ADDR_SHORT_TTL) != 0) { 
                    /* Was picked up opportunistacilly */
                    /* Convert to normal entry  */
                    pMatch->Flags &= ~BAC_ADDR_SHORT_TTL;
                    /* And give it a decent time to live */
                    pMatch->TimeToLive = BAC_ADDR_LONG_TIME;
                }
            }
            /* True if bound, false if bind request outstanding */
            return (found); 
        }
    }

    /* Not there already so look for a free entry to put it in */
    /* existing device - update address info if currently bound */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_RESERVED)) == 0) {
            /* In use and awaiting binding */
            pMatch->Flags = (uint8_t)(BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ);
            pMatch->device_id = device_id;
            /* No point in leaving bind requests in for long haul */
            pMatch->TimeToLive = BAC_ADDR_SHORT_TIME;
            /* now would be a good time to do a Who-Is request */
            return (false);
        }
    }

    /* No free entries, See if we can squeeze it in by dropping an existing one
     */
    pMatch = address_remove_oldest();
    if (pMatch != NULL) {
        pMatch->Flags = (uint8_t)(BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ);
        pMatch->device_id = device_id;
        /* No point in leaving bind requests in for long haul */
        pMatch->TimeToLive = BAC_ADDR_SHORT_TIME;
    }
    return (false);
}

/**
 * Check if the device is in the list. If yes, return the values.
 * Otherwise add the device to the list.
 * Returns true if device is already bound.
 * Also returns the address and max apdu if already bound.
 *
 * @param device_id  Pointer to the device id variable for return.
 * @param max_apdu  Max APDU size of the device.
 * @param src  Pointer to the BACnet address.
 *
 * @return true if device is already bound
 */
bool address_bind_request(
    uint32_t device_id, unsigned *max_apdu, BACNET_ADDRESS *src)
{
    return address_device_bind_request(device_id, NULL, max_apdu, src);
}

/**
 * For an existing device add a binding.
 *
 * @param device_id  Pointer to the device id variable for return.
 * @param max_apdu  Max APDU size of the device.
 * @param src  Pointer to the BACnet address.
 */
void address_add_binding(
    uint32_t device_id, unsigned max_apdu, BACNET_ADDRESS *src)
{
    struct Address_Cache_Entry *pMatch;
    unsigned index;

    /* existing device or bind request - update address */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & BAC_ADDR_IN_USE) != 0) &&
            (pMatch->device_id == device_id)) {
            bacnet_address_copy(&pMatch->address, src);
            pMatch->max_apdu = max_apdu;
            /* Clear bind request flag in case it was set */
            pMatch->Flags &= ~BAC_ADDR_BIND_REQ;
            /* Only update TTL if not static */
            if ((pMatch->Flags & BAC_ADDR_STATIC) == 0) {
                /* and set it on a long fuse */
                pMatch->TimeToLive = BAC_ADDR_LONG_TIME;
            }
            break;
        }
    }
    return;
}

/**
 * Return the device information from the given index in the table.
 *
 * @param index  Table index [0..MAX_ADDRESS_CACHE-1]
 * @param device_id  Pointer to the variable taking the device id.
 * @param device_ttl  Pointer to the variable taking the Time To Life for the
 * device.
 * @param max_apdu  Pointer to the variable taking the max APDU size of the
 * device.
 * @param src  Pointer to the BACnet address.
 *
 * @return true/false
 */
bool address_device_get_by_index(unsigned index,
    uint32_t *device_id,
    uint32_t *device_ttl,
    unsigned *max_apdu,
    BACNET_ADDRESS *src)
{
    struct Address_Cache_Entry *pMatch;
    bool found = false; /* return value */

    if (index < MAX_ADDRESS_CACHE) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) ==
            BAC_ADDR_IN_USE) {
            if (src) {
                bacnet_address_copy(src, &pMatch->address);
            }
            if (device_id) {
                *device_id = pMatch->device_id;
            }
            if (max_apdu) {
                *max_apdu = pMatch->max_apdu;
            }
            if (device_ttl) {
                *device_ttl = pMatch->TimeToLive;
            }
            found = true;
        }
    }

    return found;
}

/**
 * Return the device information from the given index in the table.
 *
 * @param index  Table index [0..MAX_ADDRESS_CACHE-1]
 * @param device_id  Pointer to the variable taking the device id.
 * @param max_apdu  Pointer to the variable taking the max APDU size of the
 * device.
 * @param src  Pointer to the BACnet address.
 *
 * @return true/false
 */
bool address_get_by_index(unsigned index,
    uint32_t *device_id,
    unsigned *max_apdu,
    BACNET_ADDRESS *src)
{
    return address_device_get_by_index(index, device_id, NULL, max_apdu, src);
}

/**
 * Return the count of cached addresses.
 *
 * @return A value between zero and MAX_ADDRESS_CACHE.
 */
unsigned address_count(void)
{
    struct Address_Cache_Entry *pMatch;
    unsigned count = 0; /* return value */
    unsigned index;

    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        /* Only count bound entries */
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) ==
            BAC_ADDR_IN_USE) {
            count++;
        }
    }

    return count;
}

/**
 * Build a list of the current bindings for the device address binding
 * property. Basically encode the address list to be send out.
 *
 * @param apdu  Pointer to the APDU
 * @param apdu_len  Remaining buffer size.
 *
 * @return Count of encoded bytes.
 */
int address_list_encode(uint8_t *apdu, unsigned apdu_len)
{
    int iLen = 0;
    struct Address_Cache_Entry *pMatch;
    BACNET_OCTET_STRING MAC_Address;
    unsigned index;

    /* Look for matching address. */
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) ==
            BAC_ADDR_IN_USE) {
            iLen += encode_application_object_id(
                &apdu[iLen], OBJECT_DEVICE, pMatch->device_id);
            iLen +=
                encode_application_unsigned(&apdu[iLen], pMatch->address.net);
            if ((unsigned)iLen >= apdu_len) {
                break;
            }

            /* pick the appropriate type of entry from the cache */

            if (pMatch->address.len != 0) {
                /* BAC */
                if ((unsigned)(iLen + pMatch->address.len) >= apdu_len) {
                    break;
                }
                octetstring_init(
                    &MAC_Address, pMatch->address.adr, pMatch->address.len);
                iLen +=
                    encode_application_octet_string(&apdu[iLen], &MAC_Address);
            } else {
                /* MAC*/
                if ((unsigned)(iLen + pMatch->address.mac_len) >= apdu_len) {
                    break;
                }
                octetstring_init(
                    &MAC_Address, pMatch->address.mac, pMatch->address.mac_len);
                iLen +=
                    encode_application_octet_string(&apdu[iLen], &MAC_Address);
            }
            /* Any space left? */
            if ((unsigned)iLen >= apdu_len) {
                break;
            }
        }
    }

    return (iLen);
}

/**
 * Build a list of the current bindings for the device address binding
 * property as required for the ReadsRange functionality.
 * We assume we only get called for "Read All" or "By Position" requests.
 *
 * We need to treat the address cache as a contiguous array but in reality
 * it could be sparsely populated. We can get the count but we can only
 * extract entries by doing a linear scan starting from the first entry in
 * the cache and picking them off one by one.
 *
 * We do assume the list cannot change whilst we are accessing it so would
 * not be multithread safe if there are other tasks that change the cache.
 *
 * We take the simple approach here to filling the buffer by taking a max   *
 * size for a single entry and then stopping if there is less than that
 * left in the buffer. You could build each entry in a seperate buffer and
 * determine the exact length before copying but this is time consuming,
 * requires more memory and would probably only let you sqeeeze one more
 * entry in on occasion. The value is calculated as 5 bytes for the device
 * ID + 3 bytes for the network number and nine bytes for the MAC address
 * oct string to give 17 bytes (the minimum possible is 5 + 2 + 3 = 10).
 *
 * @param apdu  Pointer to the encode buffer.
 * @param pRequest  Pointer to the read_range request structure.
 *
 * @return Bytes encoded.
 */
#define ACACHE_MAX_ENC 17 /* Maximum size of encoded cache entry, see above */

int rr_address_list_encode(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest)
{
    int iLen = 0;
    int32_t iTemp = 0;
    struct Address_Cache_Entry *pMatch = NULL;
    BACNET_OCTET_STRING MAC_Address;
    uint32_t uiTotal = 0; /* Number of bound entries in the cache */
    uint32_t uiIndex = 0; /* Current entry number */
    uint32_t uiFirst = 0; /* Entry number we started encoding from */
    uint32_t uiLast = 0; /* Entry number we finished encoding on */
    uint32_t uiTarget = 0; /* Last entry we are required to encode */
    uint32_t uiRemaining = 0; /* Amount of unused space in packet */

    if ((!pRequest) || (!apdu)) {
        return 0;
    }

    /* Initialise result flags to all false */
    bitstring_init(&pRequest->ResultFlags);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, false);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, false);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, false);
    /* See how much space we have */
    uiRemaining = (uint32_t)(MAX_APDU - pRequest->Overhead);

    pRequest->ItemCount = 0; /* Start out with nothing */
    uiTotal = address_count(); /* What do we have to work with here ? */
    if (uiTotal == 0) { /* Bail out now if not. */
        return (0);
    }

    if (pRequest->RequestType == RR_READ_ALL) {
        /*
         * Read all the array or as much as will fit in the buffer by selecting
         * a range that covers the whole list and falling through to the next
         * section of code
         */
        pRequest->Count = uiTotal; /* Full list */
        pRequest->Range.RefIndex = 1; /* Starting at the beginning */
    }

    if (pRequest->Count <
        0) { /* negative count means work from index backwards */
        /*
         * Convert from end index/negative count to
         * start index/positive count and then process as
         * normal. This assumes that the order to return items
         * is always first to last, if this is not true we will
         * have to handle this differently.
         *
         * Note: We need to be careful about how we convert these
         * values due to the mix of signed and unsigned types - don't
         * try to optimise the code unless you understand all the
         * implications of the data type conversions!
         */

        iTemp = pRequest->Range.RefIndex; /* pull out and convert to signed */
        iTemp +=
            pRequest->Count + 1; /* Adjust backwards, remember count is -ve */
        if (iTemp <
            1) { /* if count is too much, return from 1 to start index */
            pRequest->Count = pRequest->Range.RefIndex;
            pRequest->Range.RefIndex = 1;
        } else { /* Otherwise adjust the start index and make count +ve */
            pRequest->Range.RefIndex = iTemp;
            pRequest->Count = -pRequest->Count;
        }
    }

    /* From here on in we only have a starting point and a positive count */

    if (pRequest->Range.RefIndex > uiTotal) { 
        /* Nothing to return as we are past the end of the list */
        return (0);
    }
    /* Index of last required entry */
    uiTarget = pRequest->Range.RefIndex + pRequest->Count - 1; 
    if (uiTarget > uiTotal) { /* Capped at end of list if necessary */
        uiTarget = uiTotal;
    }

    pMatch = Address_Cache;
    uiIndex = 1;
    while ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) !=
        BAC_ADDR_IN_USE) { /* Find first bound entry */
        pMatch++;
        /* Shall not happen as the count has been checked first. */
        if (pMatch > &Address_Cache[MAX_ADDRESS_CACHE - 1]) {
        	/* Issue with the table. */
            return (0); 
        }
    }

    /* Seek to start position */
    while (uiIndex != pRequest->Range.RefIndex) {
        if ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) ==
            BAC_ADDR_IN_USE) { 
            /* Only count bound entries */
            pMatch++;
            uiIndex++;
        } else {
            pMatch++;
        }
        /* Shall not happen as the count has been checked first. */
        if (pMatch > &Address_Cache[MAX_ADDRESS_CACHE - 1]) {
        	/* Issue with the table. */
            return (0); 
        }
    }

    uiFirst = uiIndex; /* Record where we started from */
    while (uiIndex <= uiTarget) {
        if (uiRemaining < ACACHE_MAX_ENC) {
            /*
             * Can't fit any more in! We just set the result flag to say there
             * was more and drop out of the loop early
             */
            bitstring_set_bit(
                &pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, true);
            break;
        }
        iTemp = (int32_t)encode_application_object_id(
            &apdu[iLen], OBJECT_DEVICE, pMatch->device_id);
        iTemp += encode_application_unsigned(
            &apdu[iLen + iTemp], pMatch->address.net);

        /* pick the appropriate type of entry from the cache */
        if (pMatch->address.len != 0) {
            octetstring_init(
                &MAC_Address, pMatch->address.adr, pMatch->address.len);
            iTemp += encode_application_octet_string(
                &apdu[iLen + iTemp], &MAC_Address);
        } else {
            octetstring_init(
                &MAC_Address, pMatch->address.mac, pMatch->address.mac_len);
            iTemp += encode_application_octet_string(
                &apdu[iLen + iTemp], &MAC_Address);
        }
        /* Reduce the remaining space */
        uiRemaining -= iTemp; 
        /* and increase the length consumed */
        iLen += iTemp; 
        /* Record the last entry encoded */
        uiLast = uiIndex; 
        /* and get ready for next one */
        uiIndex++; 
        pMatch++;
        /* Chalk up another one for the response count */
        pRequest->ItemCount++; 

        while ((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_BIND_REQ)) !=
            BAC_ADDR_IN_USE) { 
            /* Find next bound entry */
            pMatch++;
            /* Can normally not happen. */
            if (pMatch > &Address_Cache[MAX_ADDRESS_CACHE - 1]) {
            	/* Issue with the table. */
                return (0); 
            }
        }
    }
    /* Set remaining result flags if necessary */
    if (uiFirst == 1) {
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
    }

    if (uiLast == uiTotal) {
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);
    }

    return (iLen);
}

/**
 * Scan the cache and eliminate any expired entries. Should be called
 * periodically to ensure the cache is managed correctly. If this function
 * is never called at all the whole cache is effectivly rendered static and
 * entries never expire unless explictely deleted.
 *
 * @param uSeconds  Approximate number of seconds since last call to this
 * function
 */
void address_cache_timer(uint16_t uSeconds)
{
    struct Address_Cache_Entry *pMatch;
    unsigned index;
    
    for (index = 0; index < MAX_ADDRESS_CACHE; index++) {
        pMatch = &Address_Cache[index];
        if (((pMatch->Flags & (BAC_ADDR_IN_USE | BAC_ADDR_RESERVED)) != 0) &&
            ((pMatch->Flags & BAC_ADDR_STATIC) ==
                0)) { /* Check all entries holding a slot except statics
                       */
            if (pMatch->TimeToLive >= uSeconds) {
                pMatch->TimeToLive -= uSeconds;
            } else {
                pMatch->Flags = 0;
            }
        }
    }
}

#ifdef BAC_TEST
#include <assert.h>
#include <string.h>
#include "ctest.h"

static void set_address(unsigned index, BACNET_ADDRESS *dest)
{
    unsigned i;

    for (i = 0; i < MAX_MAC_LEN; i++) {
        dest->mac[i] = index;
    }
    dest->mac_len = MAX_MAC_LEN;
    dest->net = 7;
    dest->len = MAX_MAC_LEN;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        dest->adr[i] = index;
    }
}

static void set_file_address(const char *pFilename,
    uint32_t device_id,
    BACNET_ADDRESS *dest,
    uint16_t max_apdu)
{
    unsigned i;
    FILE *pFile = NULL;

    pFile = fopen(pFilename, "w");

    if (pFile) {
        fprintf(pFile, "%lu ", (long unsigned int)device_id);
        for (i = 0; i < dest->mac_len; i++) {
            fprintf(pFile, "%02x", dest->mac[i]);
            if ((i + 1) < dest->mac_len) {
                fprintf(pFile, ":");
            }
        }
        fprintf(pFile, " %hu ", dest->net);
        if (dest->net) {
            for (i = 0; i < dest->len; i++) {
                fprintf(pFile, "%02x", dest->adr[i]);
                if ((i + 1) < dest->len) {
                    fprintf(pFile, ":");
                }
            }
        } else {
            fprintf(pFile, "0");
        }
        fprintf(pFile, " %hu\n", max_apdu);
        fclose(pFile);
    }
}

#ifdef BACNET_ADDRESS_CACHE_FILE
void testAddressFile(Test *pTest)
{
    BACNET_ADDRESS src = { 0 };
    uint32_t device_id = 0;
    unsigned max_apdu = 480;
    BACNET_ADDRESS test_address = { 0 };
    unsigned test_max_apdu = 0;

    /* create a fake address */
    device_id = 55555;
    src.mac_len = 1;
    src.mac[0] = 25;
    src.net = 0;
    src.adr[0] = 0;
    max_apdu = 50;
    set_file_address(Address_Cache_Filename, device_id, &src, max_apdu);
    /* retrieve it from the file, and see if we can find it */
    address_file_init(Address_Cache_Filename);
    ct_test(
        pTest, address_get_by_device(device_id, &test_max_apdu, &test_address));
    ct_test(pTest, test_max_apdu == max_apdu);
    ct_test(pTest, bacnet_address_same(&test_address, &src));

    /* create a fake address */
    device_id = 55555;
    src.mac_len = 6;
    src.mac[0] = 0xC0;
    src.mac[1] = 0xA8;
    src.mac[2] = 0x00;
    src.mac[3] = 0x18;
    src.mac[4] = 0xBA;
    src.mac[5] = 0xC0;
    src.net = 26001;
    src.len = 1;
    src.adr[0] = 25;
    max_apdu = 50;
    set_file_address(Address_Cache_Filename, device_id, &src, max_apdu);
    /* retrieve it from the file, and see if we can find it */
    address_file_init(Address_Cache_Filename);
    ct_test(
        pTest, address_get_by_device(device_id, &test_max_apdu, &test_address));
    ct_test(pTest, test_max_apdu == max_apdu);
    ct_test(pTest, bacnet_address_same(&test_address, &src));
}
#endif

void testAddress(Test *pTest)
{
    unsigned i, count;
    BACNET_ADDRESS src;
    uint32_t device_id = 0;
    unsigned max_apdu = 480;
    BACNET_ADDRESS test_address;
    uint32_t test_device_id = 0;
    unsigned test_max_apdu = 0;

    /* create a fake address database */
    for (i = 0; i < MAX_ADDRESS_CACHE; i++) {
        set_address(i, &src);
        device_id = i * 255;
        address_add(device_id, max_apdu, &src);
        count = address_count();
        ct_test(pTest, count == (i + 1));
    }

    for (i = 0; i < MAX_ADDRESS_CACHE; i++) {
        device_id = i * 255;
        set_address(i, &src);
        /* test the lookup by device id */
        ct_test(pTest,
            address_get_by_device(device_id, &test_max_apdu, &test_address));
        ct_test(pTest, test_max_apdu == max_apdu);
        ct_test(pTest, bacnet_address_same(&test_address, &src));
        ct_test(pTest,
            address_get_by_index(
                i, &test_device_id, &test_max_apdu, &test_address));
        ct_test(pTest, test_device_id == device_id);
        ct_test(pTest, test_max_apdu == max_apdu);
        ct_test(pTest, bacnet_address_same(&test_address, &src));
        ct_test(pTest, address_count() == MAX_ADDRESS_CACHE);
        /* test the lookup by MAC */
        ct_test(pTest, address_get_device_id(&src, &test_device_id));
        ct_test(pTest, test_device_id == device_id);
    }

    for (i = 0; i < MAX_ADDRESS_CACHE; i++) {
        device_id = i * 255;
        address_remove_device(device_id);
        ct_test(pTest,
            !address_get_by_device(device_id, &test_max_apdu, &test_address));
        count = address_count();
        ct_test(pTest, count == (MAX_ADDRESS_CACHE - i - 1));
    }
}

#ifdef TEST_ADDRESS
int main(void)
{
    Test *pTest;
    bool rc;

    pTest = ct_create("BACnet Address", NULL);
    /* individual tests */
    rc = ct_addTestFunction(pTest, testAddress);
    assert(rc);
#ifdef BACNET_ADDRESS_CACHE_FILE
    rc = ct_addTestFunction(pTest, testAddressFile);
    assert(rc);
#endif

    ct_setStream(pTest, stdout);
    ct_run(pTest);
    (void)ct_report(pTest);
    ct_destroy(pTest);

    return 0;
}
#endif /* TEST_ADDRESS */
#endif /* BAC_TEST */
