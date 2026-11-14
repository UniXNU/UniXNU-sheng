/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercise the GLINK byte order against an independent HMAC implementation. */
#include "../ShengChargeAuth.h"
#include <CommonCrypto/CommonHMAC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, const char *message) {if (!ok) {fprintf(stderr, "%s\n", message); exit(1);}}
static uint32_t word(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;}
static void store(uint8_t *p, uint32_t value) {for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(value>>(8*i));}
static void hmac(const uint8_t *key, uint32_t n, const uint8_t *message, uint32_t bytes, uint8_t digest[32]) {
    CCHmac(kCCHmacAlgSHA256, key, n, message, bytes, digest);
}
static void random_bytes(void *out, uint32_t bytes) {for (uint32_t i=0;i<bytes;i++) ((uint8_t *)out)[i]=(uint8_t)(17*i);}
static const uint8_t gauge_key[32] = {
    0x45,0xf0,0xf5,0xa0,0x23,0xb0,0x9a,0x74,0x37,0xd9,0xb8,0x5a,0x6e,0xa2,0x03,0x35,
    0x39,0x32,0x1d,0x54,0x37,0xb9,0x54,0x3f,0x3f,0x22,0x92,0xe0,0xc9,0x66,0xd6,0x75
};
static const uint8_t adapter_key[32] = {
    0x5c,0x4d,0x41,0x79,0xda,0x15,0x01,0xed,0x11,0x74,0x74,0x92,0xe2,0x60,0x83,0xd9,
    0x6b,0x8f,0xbc,0x73,0xc8,0x2e,0x7b,0xdf,0x5d,0x32,0x55,0xc2,0xa0,0x36,0xa1,0xdc
};
struct firmware {
    uint8_t gauge_challenge[2][32], adapter_challenge[16];
    uint32_t gauges, verified, pd_verified, vdm;
    int corrupt, non_xiaomi;
};
static unsigned answer(struct firmware *f, const uint8_t *request, unsigned bytes, uint8_t *reply) {
    const uint32_t opcode=word(request+8), property=word(request+(bytes==24?16:12));
    memset(reply,0,52); store(reply,32778); store(reply+4,1); store(reply+8,opcode); store(reply+12,property);
    if (opcode==0x51) {
        if (property==1) memcpy(f->gauge_challenge[request[48]],request+16,32);
        else if (property==3 || property==0xa5) f->gauges|=property==3?1:2;
        else if (property>=0x0d && property<=0x11) {
            f->vdm=property-0x0c;
            if (property==0x10) {
                const uint8_t seed_wire[16]={0x05,0x77,0xe9,0xd6,0xf4,0xf5,0xc7,0x06,0x00,0x40,0x7a,0xdf,0xb8,0x67,0x74,0x8c};
                require(!memcmp(request+16,seed_wire,16),"MiPPS session seed word order differs from Linux");
            }
            if (property==0x11)
                for (unsigned i=0;i<16;i++) f->adapter_challenge[i]=request[16+(i&~3u)+3-(i&3u)];
        } else if (property==0x12) f->verified=word(request+20);
        else if (property==0x14) f->vdm=8;
        else if (property==0x18) f->pd_verified=word(request+20);
        return 24;
    }
    if (property==1) {
        hmac(gauge_key,32,f->gauge_challenge[request[48]],32,reply+16); return 48;
    }
    if (property==0x11) {
        uint8_t message[20], digest[32];
        memcpy(message,f->adapter_challenge,16);
        memcpy(message+16,"\x12\x34\x56\x78",4);
        hmac(adapter_key,32,message,20,digest);
        if (f->corrupt) digest[0]^=1;
        for (unsigned i=0;i<16;i++) reply[16+i]=digest[(i&~3u)+3-(i&3u)];
        return 32;
    }
    uint32_t value=0;
    if (property==3) value=f->gauges&1;
    else if (property==0xa5) value=(f->gauges>>1)&1;
    else if (property==0x17) value=f->non_xiaomi?0x8086:0x2717;
    else if (property==0x19) value=0xc0dc213c;
    else if (property==0x1a) value=f->vdm;
    else if (property==0x16) value=0x12345678;
    store(reply+16,value); return 24;
}
static void run(struct firmware *f) {
    ShengChargeAuth a;
    const ShengChargeCrypto crypto={random_bytes,hmac};
    sheng_charge_auth_init(&a,&crypto); sheng_charge_auth_observe(&a,1,8,0);
    unsigned guard=0; uint64_t now=0;
    while (a.state!=ChargeDone && a.state!=ChargeFailed && guard++<500) {
        uint8_t request[52], reply[52];
        if (a.state==ChargeHostRole) a.host_ready=1;
        if (now<a.due_ms) now=a.due_ms;
        unsigned bytes=sheng_charge_auth_next(&a,now,request);
        if (!bytes) {now++; continue;}
        unsigned n=answer(f,request,bytes,reply);
        if (guard==1) {
            store(reply+12,0xa5);
            require(!sheng_charge_auth_receive(&a,reply,n,now) && a.pending,"Unrelated GLINK answer consumed the authentication request");
            store(reply+12,3);
        }
        require(sheng_charge_auth_receive(&a,reply,n,now),"Authentication reply was not accepted");
    }
    require(guard<500 && f->gauges==3,"Fuel-gauge authentication did not complete");
    require(f->pd_verified==(uint32_t)(!f->corrupt && !f->non_xiaomi) && f->verified==f->pd_verified,
            "Adapter authentication published an incorrect verification result");
    require(a.state==(uint32_t)(f->corrupt?ChargeFailed:ChargeDone),"Authentication did not reach its expected final state");
    sheng_charge_auth_observe(&a,0,0,now);
    sheng_charge_auth_observe(&a,1,1,now);
    require(!a.online && !a.pending,"Reconnecting to a computer retained the prior PD authentication");
}
int main(void) {
    struct firmware authentic={0}, mismatch={.corrupt=1}, standard_pps={.non_xiaomi=1};
    run(&authentic); run(&mismatch); run(&standard_pps);
    puts("PASS charge authentication: Linux word order, valid/mismatched HMAC, standard PPS and reconnect");
}
