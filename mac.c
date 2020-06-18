/*
Copyright (c) 2018 by Clara Dô and Weronika Kolodziejak

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "rfc6234/sha.h"
#include "BLAKE2/ref/blake2.h"

#include "babeld.h"
#include "interface.h"
#include "neighbour.h"
#include "util.h"
#include "mac.h"
#include "configuration.h"
#include "message.h"

struct key **keys = NULL;
int numkeys = 0, maxkeys = 0;

struct key *
find_key(const char *name)
{
    int i;
    for(i = 0; i < numkeys; i++) {
        if(strcmp(keys[i]->name, name) == 0)
            return retain_key(keys[i]);
    }
    return NULL;
}

struct key *
retain_key(struct key *key)
{
    assert(key->ref_count < 0xffff);
    key->ref_count++;
    return key;
}

void
release_key(struct key *key)
{
    assert(key->ref_count > 0);
    key->ref_count--;
}

struct key *
add_key(char *name, int algorithm, int len, unsigned char *value)
{
    struct key *key;

    assert(value != NULL);

    key = find_key(name);
    if(key) {
        key->algorithm = algorithm;
        key->len = len;
        memcpy(key->value, value, len);
        return key;
    }

    if(numkeys >= maxkeys) {
        struct key **new_keys;
        int n = maxkeys < 1 ? 8 : 2 * maxkeys;
        new_keys = realloc(keys, n * sizeof(struct key*));
        if(new_keys == NULL)
            return NULL;
        maxkeys = n;
        keys = new_keys;
    }

    key = calloc(1, sizeof(struct key));
    if(key == NULL)
        return NULL;
    memcpy(key->name, name, MAX_KEY_NAME_LEN);
    key->algorithm = algorithm;
    key->len = len;
    memcpy(key->value, value, len);

    keys[numkeys++] = key;
    return key;
}

static int
compute_mac(const unsigned char *src, const unsigned char *dst,
            const unsigned char *packet_header,
            const unsigned char *body, int bodylen, const struct key *key,
            unsigned char *mac_return)
{
    unsigned char port[2];
    int rc;

    DO_HTONS(port, (unsigned short)protocol_port);
    switch(key->algorithm) {
    case MAC_ALGORITHM_HMAC_SHA256: {
        /* Reference hmac-sha functions weigth up babeld by 32Kb-36Kb,
         * so we roll our own! */
        unsigned char pad[SHA256_Message_Block_Size], ihash[SHA256HashSize];
        SHA256Context c;
        int i;

        for(i = 0; i < key->len; i++)
            pad[i] = key->value[i] ^ 0x36;
        for(; i < (int)sizeof(pad); i++)
            pad[i] = 0x36;

        rc = SHA256Reset(&c);
        if(rc < 0)
            return -1;

        rc = SHA256Input(&c, pad, sizeof(pad));
        if(rc < 0)
            return -1;
        rc = SHA256Input(&c, src, 16);
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, port, 2);
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, dst, 16);
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, port, 2);
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, packet_header, 4);
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, body, bodylen);
        if(rc != 0)
            return -1;
        rc = SHA256Result(&c, ihash);
        if(rc != 0)
            return -1;

        for(i = 0; i < key->len; i++)
            pad[i] = key->value[i] ^ 0x5c;
        for(; i < (int)sizeof(pad); i++)
            pad[i] = 0x5c;

        rc = SHA256Reset(&c);
        if(rc != 0)
            return -1;

        rc = SHA256Input(&c, pad, sizeof(pad));
        if(rc != 0)
            return -1;
        rc = SHA256Input(&c, ihash, sizeof(ihash));
        if(rc != 0)
            return -1;
        rc = SHA256Result(&c, mac_return);
        if(rc < 0)
            return -1;

        return SHA256HashSize;
    }
    case MAC_ALGORITHM_BLAKE2S: {
        blake2s_state s;
        rc = blake2s_init_key(&s, BLAKE2S_OUTBYTES, key->value, key->len);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, src, 16);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, port, 2);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, dst, 16);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, port, 2);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, packet_header, 4);
        if(rc < 0)
            return -1;
        rc = blake2s_update(&s, body, bodylen);
        if(rc < 0)
            return -1;
        rc = blake2s_final(&s, mac_return, BLAKE2S_OUTBYTES);
        if(rc < 0)
            return -1;

        return BLAKE2S_OUTBYTES;
    }
    default:
        return -1;
    }
}

int
sign_packet(struct buffered *buf, const struct interface *ifp,
            const unsigned char *packet_header)
{
    int maclen;
    int i = buf->len;
    unsigned char *dst = buf->sin6.sin6_addr.s6_addr;
    unsigned char *src;

    if(ifp->numll < 1) {
        fprintf(stderr, "sign_packet: no link-local address.\n");
        return -1;
    }
    src = ifp->ll[0];

    if(buf->len + MAX_MAC_SPACE > buf->size) {
        fprintf(stderr, "sign_packet: buffer overflow.\n");
        return -1;
    }

    maclen = compute_mac(src, dst, packet_header,
                         buf->buf, buf->len, ifp->key,
                         buf->buf + i + 2);
    if(maclen < 0)
        return -1;
    buf->buf[i++] = MESSAGE_MAC;
    buf->buf[i++] = maclen;
    i += maclen;
    return i;
}


static int
compare_macs(const unsigned char *src, const unsigned char *dst,
             const unsigned char *packet, int bodylen,
             const unsigned char *mac, int maclen,
             const struct key *key)
{
    unsigned char buf[MAX_DIGEST_LEN];
    int len;

    len = compute_mac(src, dst, packet, packet + 4, bodylen, key, buf);
    return len == maclen && (memcmp(buf, mac, maclen) == 0);
}

int
verify_packet(const unsigned char *packet, int packetlen, int bodylen,
              const unsigned char *src, const unsigned char *dst,
              const struct interface *ifp)
{
    int i = bodylen + 4;
    int len;
    int rc = -1;

    debugf("verify_packet %s -> %s\n",
           format_address(src), format_address(dst));
    while(i < packetlen) {
        if(i + 2 > packetlen) {
            fprintf(stderr, "Received truncated message.\n");
            break;
        }
        len = packet[i + 1];
        if(packet[i] == MESSAGE_MAC) {
            int ok;
            if(i + len + 2 > packetlen) {
                fprintf(stderr, "Received truncated message.\n");
                return -1;
            }
            ok = compare_macs(src, dst, packet, bodylen,
                              packet + i + 2, len, ifp->key);
            if(ok)
                return 1;
            rc = 0;
        }
        i += len + 2;
    }
    return rc;
}