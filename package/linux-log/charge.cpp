/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Linux sysfs adapter for the board's shared authentication state machine. */
#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoSha256.h"
extern "C" {
#include "../ShengChargeAuth.h"
void *memcpy(void *destination, const void *source, size_t bytes) {
    auto *d = static_cast<uint8_t *>(destination); auto *s = static_cast<const uint8_t *>(source);
    for (size_t i = 0; i < bytes; ++i) d[i] = s[i];
    return destination;
}
void *memset(void *destination, int value, size_t bytes) {
    auto *d = static_cast<uint8_t *>(destination);
    for (size_t i = 0; i < bytes; ++i) d[i] = static_cast<uint8_t>(value);
    return destination;
}
}
static long syscall(long number, long a = 0, long b = 0, long c = 0, long d = 0) {
    register long x8 asm("x8") = number, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
static size_t length(const char *s) {size_t n = 0; while (s[n]) ++n; return n;}
static bool same(const char *a, const char *b) {size_t i = 0; while (a[i] && a[i] == b[i]) ++i; return a[i] == b[i];}
static bool contains(const char *s, const char *part) {
    for (size_t i = 0; s[i]; ++i) {size_t j = 0; while (part[j] && s[i+j] == part[j]) ++j; if (!part[j]) return true;}
    return false;
}
static bool copy(char *out, size_t size, const char *text) {
    size_t n = length(text); if (n >= size) return false; memcpy(out, text, n + 1); return true;
}
static bool path(char *out, const char *base, const char *name) {
    size_t a = length(base), b = length(name); if (a + b + 2 > 512) return false;
    memcpy(out, base, a); out[a++] = '/'; memcpy(out + a, name, b + 1); return true;
}
static bool write_all(int fd, const void *bytes, size_t count) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    while (count) {long n = syscall(64, fd, (long)p, count); if (n <= 0) return false; p += n; count -= n;}
    return true;
}
static int log_fd = -1;
static void emit(const char *s) {write_all(1, s, length(s)); if (log_fd >= 0) write_all(log_fd, s, length(s));}
static void integer(int64_t value) {
    char text[32]; unsigned n = 0; uint64_t v = value < 0 ? -(uint64_t)value : (uint64_t)value;
    do {text[n++] = (char)('0' + v % 10); v /= 10;} while (v);
    if (value < 0) text[n++] = '-';
    for (unsigned i = 0; i < n / 2; ++i) {char c = text[i]; text[i] = text[n-i-1]; text[n-i-1] = c;}
    text[n] = 0; emit(text);
}
[[noreturn]] static void finish(int status) {if (log_fd >= 0) syscall(57, log_fd); syscall(93, status); for (;;) {}}
static uint32_t word(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;}
static void store(uint8_t *p, uint32_t value) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8*i));}
struct timespec {int64_t sec, ns;};
static uint64_t now_ms() {timespec t; if (syscall(113, 1, (long)&t)) finish(3); return t.sec * 1000u + t.ns / 1000000u;}
static void sleep_ms(uint64_t ms) {timespec t = {(int64_t)(ms / 1000), (int64_t)(ms % 1000 * 1000000)}; syscall(101, (long)&t, 0);}
static int read_file(const char *file, char *out, unsigned capacity) {
    long fd = syscall(56, -100, (long)file, 0); if (fd < 0) return (int)fd;
    long n = syscall(63, fd, (long)out, capacity - 1); syscall(57, fd);
    if (n < 0) return (int)n;
    while (n && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) --n;
    out[n] = 0; return (int)n;
}
static int read_node(const char *root, const char *name, char *out, unsigned size) {
    char file[512]; if (!path(file, root, name)) return -36; return read_file(file, out, size);
}
static int read_binary(const char *root, const char *name, uint8_t *out, unsigned size) {
    char file[512]; if (!path(file, root, name)) return -36;
    long fd = syscall(56,-100,(long)file,0); if (fd < 0) return (int)fd;
    long bytes = syscall(63,fd,(long)out,size); syscall(57,fd); return (int)bytes;
}
static uint32_t be_word(const uint8_t *p) {return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];}
static bool dt_word(const char *root, const char *name, uint32_t *out) {
    uint8_t bytes[8]; if (read_binary(root,name,bytes,sizeof bytes) != 4) return false;
    *out = be_word(bytes); return true;
}
static bool dt_pair(const char *root, const char *name, uint32_t first, uint32_t second) {
    uint8_t bytes[12]; return read_binary(root,name,bytes,sizeof bytes) == 8 && be_word(bytes) == first && be_word(bytes+4) == second;
}
static int arm_fastboot() {
    if (syscall(174) != 0) {emit("[charge safety] root required\n"); return 2;}
    const char *tree = "/sys/firmware/devicetree/base";
    char text[128], reboot[512]; uint32_t cell = 0, mode = 0;
    if (read_node(tree,"compatible",text,sizeof text) < 0 || !same(text,"xiaomi,sheng") ||
        !path(reboot,tree,"reboot-mode") || !dt_word(reboot,"nvmem-cells",&cell) || !cell ||
        !dt_word(reboot,"mode-bootloader",&mode) || mode != 2) {
        emit("[charge safety] sheng reboot-mode cell unavailable\n"); return 2;
    }
    const char *devices = "/sys/bus/nvmem/devices";
    long directory = syscall(56,-100,(long)devices,0);
    if (directory < 0) {emit("[charge safety] nvmem providers unavailable\n"); return 2;}
    uint8_t buffer[4096]; char selected[512] = {}; long bytes; unsigned matches = 0;
    while ((bytes = syscall(61,directory,(long)buffer,sizeof buffer)) > 0) {
        for (long at = 0; at+20 <= bytes;) {
            uint16_t record = buffer[at+16] | (uint16_t)buffer[at+17]<<8;
            if (record < 20 || at+record > bytes) break;
            const char *name = (const char *)buffer+at+19;
            char provider[512], node[512], reason[512]; uint32_t base = 0, phandle = 0;
            if (name[0] != '.' && path(provider,devices,name) && path(node,provider,"of_node") &&
                read_node(node,"compatible",text,sizeof text) > 0 && same(text,"qcom,spmi-sdam") &&
                dt_word(node,"reg",&base) && base == 0x7100 && path(reason,node,"reboot-reason@48") &&
                dt_word(reason,"phandle",&phandle) && phandle == cell && dt_pair(reason,"reg",0x48,1) &&
                dt_pair(reason,"bits",1,7) && path(selected,provider,"nvmem")) ++matches;
            at += record;
        }
    }
    syscall(57,directory);
    if (bytes < 0 || matches != 1) {emit("[charge safety] unique SDAM reboot-reason provider unavailable\n"); return 2;}
    long fd = syscall(56,-100,(long)selected,2);
    uint8_t old = 0, got = 0;
    if (fd < 0) {emit("[charge safety] reboot reason open failed\n"); return 2;}
    bool ok = syscall(67,fd,(long)&old,1,0x48) == 1;
    const uint8_t wanted = (old & 1u) | (2u << 1);
    if (ok) ok = syscall(68,fd,(long)&wanted,1,0x48) == 1 && syscall(67,fd,(long)&got,1,0x48) == 1 && got == wanted;
    syscall(57,fd);
    if (!ok) {emit("[charge safety] reboot reason write/readback failed\n"); return 2;}
    emit("[charge safety] "); emit(selected); emit(" offset=72 old="); integer(old);
    emit(" bootloader="); integer(got); emit(" verified\n"); return 0;
}
static int write_node(const char *root, const char *name, const char *value) {
    char file[512]; if (!path(file, root, name)) return -36;
    long fd = syscall(56, -100, (long)file, 1); if (fd < 0) return (int)fd;
    long n = syscall(64, fd, (long)value, length(value)); syscall(57, fd);
    return n == (long)length(value) ? 0 : n < 0 ? (int)n : -5;
}
static bool number(const char *text, unsigned base, uint32_t *out) {
    uint64_t value = 0; bool negative = *text == '-'; if (negative) ++text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {base = 16; text += 2;}
    if (!*text) return false;
    for (; *text; ++text) {
        unsigned d = *text >= '0' && *text <= '9' ? *text - '0' : *text >= 'a' && *text <= 'f' ? *text - 'a' + 10 :
                     *text >= 'A' && *text <= 'F' ? *text - 'A' + 10 : 255;
        if (d >= base || value > (0xffffffffull - d) / base) return false;
        value = value * base + d;
    }
    *out = negative ? -(uint32_t)value : (uint32_t)value; return true;
}
static void hex(char *out, const uint8_t *data, unsigned bytes) {
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < bytes; ++i) {out[2*i] = digits[data[i] >> 4]; out[2*i+1] = digits[data[i] & 15];}
    out[2*bytes] = 0;
}
static bool unhex(uint8_t *out, const char *text, unsigned bytes) {
    if (length(text) != 2*bytes) return false;
    for (unsigned i = 0; i < bytes; ++i) {char part[3] = {text[2*i], text[2*i+1], 0}; uint32_t v; if (!number(part, 16, &v)) return false; out[i] = (uint8_t)v;}
    return true;
}
static void random_bytes(void *out, uint32_t bytes) {
    auto *p = static_cast<uint8_t *>(out);
    while (bytes) {
        long n = syscall(278, (long)p, bytes, 1);
        if (n <= 0) {emit("[charge error] kernel entropy unavailable\n"); finish(3);}
        p += n; bytes -= (uint32_t)n;
    }
}
static void hmac_sha256(const uint8_t *key, uint32_t key_bytes, const uint8_t *message, uint32_t bytes, uint8_t digest[32]) {
    if (key_bytes > 64 || bytes > 32) finish(3);
    uint8_t block[96] = {}, inner[32];
    memcpy(block, key, key_bytes);
    for (unsigned i = 0; i < 64; ++i) block[i] ^= 0x36;
    memcpy(block + 64, message, bytes); NativeAdrenoSha256::hash(block, 64 + bytes, inner);
    for (unsigned i = 0; i < 64; ++i) block[i] ^= 0x36 ^ 0x5c;
    memcpy(block + 64, inner, 32); NativeAdrenoSha256::hash(block, sizeof block, digest);
    memset(block, 0, sizeof block); memset(inner, 0, sizeof inner);
}
static const struct {uint32_t property; const char *name;} properties[] = {
    {0x03,"authentic"}, {0xa5,"slave_authentic"}, {0x16,"adapter_id"}, {0x17,"adapter_svid"}, {0x18,"pd_verifed"},
    {0x19,"pdo2"}, {0x32,"apdo_max"}, {0x33,"power_max"}, {0x2b,"fastchg_mode"}, {0x31,"quick_charge_type"},
    {0xe7,"pps_ptf"}, {0x1b,"bq2597x_chip_ok"}, {0x1c,"bq2597x_slave_chip_ok"}, {0x1d,"bq2597x_bus_current"},
    {0x1e,"bq2597x_slave_bus_current"}, {0x20,"bq2597x_bus_voltage"}, {0x23,"bq2597x_battery_voltage"}
};
static const char *property_name(uint32_t property) {
    for (const auto &p : properties) if (p.property == property) return p.name;
    return nullptr;
}
static bool find_root(char root[512]) {
    const char *candidates[] = {"/sys/class/qcom-battery", "/sys/bus/auxiliary/devices/pmic_glink.power-supply.0/xiaomi",
                               "/sys/devices/platform/pmic-glink/pmic_glink.power-supply.0/xiaomi"};
    for (const char *candidate : candidates) {
        char file[512]; path(file, candidate, "request_vdm_cmd"); long fd = syscall(56, -100, (long)file, 0);
        if (fd >= 0) {syscall(57, fd); return copy(root, 512, candidate);}
    }
    long directory = syscall(56, -100, (long)"/sys/bus/auxiliary/devices", 0);
    if (directory < 0) return false;
    uint8_t buffer[4096]; bool found = false; long bytes;
    while (!found && (bytes = syscall(61, directory, (long)buffer, sizeof buffer)) > 0) {
        for (long at = 0; at + 20 <= bytes;) {
            uint16_t record = buffer[at+16] | (uint16_t)buffer[at+17] << 8;
            if (record < 20 || at + record > bytes) break;
            const char *name = (const char *)buffer + at + 19;
            if (contains(name,"pmic_glink.power-supply")) {
                char base[512];
                if (path(base,"/sys/bus/auxiliary/devices",name) && path(root,base,"xiaomi")) {found = true; break;}
            }
            at += record;
        }
    }
    syscall(57, directory); return found;
}
static void telemetry(const char *root) {
    char value[160];
    for (const auto &p : properties) {
        int n = read_node(root, p.name, value, sizeof value);
        emit("[charge firmware] "); emit(p.name); emit("=");
        if (n >= 0) emit(value); else {emit("error:"); integer(n);}
        emit("\n");
    }
    const char *supplies[] = {"usb","battery"};
    const char *fields[] = {"online","voltage_now","current_now","current_max","input_current_limit","temp","capacity","status"};
    for (const char *supply : supplies) for (const char *field : fields) {
        char base[512]; path(base,"/sys/class/power_supply",supply);
        if (read_node(base,field,value,sizeof value) >= 0) {emit("[charge supply] "); emit(supply); emit(" "); emit(field); emit("="); emit(value); emit("\n");}
    }
}
static int source_type(const char *root) {
    char online[8], type[32];
    if (read_file("/sys/class/power_supply/usb/online",online,sizeof online) < 0 || !same(online,"1")) return 0;
    if (read_node(root,"real_type",type,sizeof type) < 0) return 0;
    return same(type,"PD") ? 6 : same(type,"PD_DRP") ? 7 : same(type,"PD_PPS") ? 8 : 1;
}
static uint64_t partner_inode() {
    uint8_t stat[160] = {};
    if (syscall(79,-100,(long)"/sys/class/typec/port0-partner",(long)stat,0)) return 0;
    uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)stat[8+i] << (8*i);
    return value;
}
static unsigned exchange(const char *root, const uint8_t *request, unsigned bytes, uint8_t reply[52]) {
    const uint32_t opcode = word(request+8), property = word(request+(bytes == 24 ? 16 : 12));
    memset(reply,0,52); store(reply,32778); store(reply+4,1); store(reply+8,opcode); store(reply+12,property);
    char text[160], payload[68]; int error = 0;
    if (bytes == 52) {
        error = write_node(root,"verify_slave_flag",request[48] ? "1" : "0");
        if (!error && opcode == 0x51) {hex(payload,request+16,32); error = write_node(root,"verify_digest",payload);}
        else if (!error) {
            error = read_node(root,"verify_digest",text,sizeof text);
            if (error >= 0 && unhex(reply+16,text,32)) return 48;
            if (error >= 0) error = -61;
        }
    } else if (bytes == 32) {
        if (opcode == 0x51) {
            uint8_t octets[16]; for (unsigned i = 0; i < 16; ++i) octets[i] = request[16+(i&~3u)+3-(i&3u)];
            payload[0] = (char)('0' + property - 0x0c); payload[1] = ','; hex(payload+2,octets,16);
            error = write_node(root,"request_vdm_cmd",payload);
        } else {
            error = read_node(root,"request_vdm_cmd",text,sizeof text);
            if (error >= 0 && text[0] == '5' && text[1] == ',' && length(text+2) == 32) {
                uint8_t octets[16];
                if (unhex(octets,text+2,16)) {for (unsigned i = 0; i < 16; ++i) reply[16+i] = octets[(i&~3u)+3-(i&3u)]; return 32;}
            }
            if (error >= 0) error = -61;
        }
    } else if (opcode == 0x51) {
        if (property >= 0x0d && property <= 0x14) {
            payload[0] = (char)('0' + property - 0x0c); payload[1] = ',';
            if (property == 0x12 || property == 0x13) hex(payload+2,request+20,4); else copy(payload+2,sizeof payload-2,"null");
            error = write_node(root,"request_vdm_cmd",payload);
        } else {
            const char *name = property_name(property);
            error = name ? write_node(root,name,word(request+20) ? "1" : "0") : -95;
        }
    } else {
        const char *name = property_name(property); uint32_t value = 0;
        error = read_node(root,property == 0x1a ? "request_vdm_cmd" : name ? name : "",text,sizeof text);
        if (error >= 0) {
            if (property == 0x1a) {unsigned i = 0; while (text[i] && text[i] != ',') ++i; text[i] = 0;}
            if (number(text,property == 0x16 || property == 0x19 ? 16 : 10,&value)) {store(reply+16,value); return 24;}
            error = -22;
        }
    }
    store(reply+20,error < 0 ? (uint32_t)-error : 0); return 24;
}
static int authenticate(const char *root) {
    if (syscall(174) != 0) {emit("[charge error] authentication requires root\n"); return 2;}
    const int type = source_type(root);
    if (type < 6) {emit("[charge auth] source-not-pd\n"); return 0;}
    ShengChargeAuth auth; const ShengChargeCrypto crypto = {random_bytes,hmac_sha256};
    sheng_charge_auth_init(&auth,&crypto); sheng_charge_auth_observe(&auth,1,type,now_ms());
    const uint64_t deadline = now_ms()+90000, partner = partner_inode();
    unsigned requests = 0, last_state = ~0u; bool role_requested = false;
    while (auth.state != ChargeDone && auth.state != ChargeFailed) {
        if (now_ms() >= deadline || requests >= 500) {emit("[charge auth] timeout\n"); return 3;}
        if (source_type(root) < 6 || (partner && partner_inode() != partner)) {emit("[charge auth] source-changed\n"); return 3;}
        if (auth.state != last_state) {emit("[charge auth] state="); integer(auth.state); emit("\n"); last_state = auth.state;}
        if (auth.state == ChargeHostRole) {
            char role[64]; int result = read_file("/sys/class/typec/port0/data_role",role,sizeof role);
            auth.host_ready = result < 0 ? result : contains(role,"[host]") ? 1 : 0;
            if (!auth.host_ready && !role_requested) {
                role_requested = true;
                auth.host_ready = write_node("/sys/class/typec/port0","data_role","host");
            }
        }
        uint64_t now = now_ms();
        if (now < auth.due_ms) {uint64_t wait = auth.due_ms-now; sleep_ms(wait > 100 ? 100 : wait); continue;}
        uint8_t request[52], reply[52]; unsigned bytes = sheng_charge_auth_next(&auth,now,request);
        if (!bytes) continue;
        ++requests; unsigned reply_bytes = exchange(root,request,bytes,reply);
        sheng_charge_auth_receive(&auth,reply,reply_bytes,now_ms());
    }
    emit("[charge auth] state="); integer(auth.state); emit(" result="); integer(auth.result);
    emit(" last="); integer(auth.last_result); emit(" gauges="); integer(auth.gauge_verified);
    emit(" adapter="); integer(auth.adapter_verified); emit(" read="); integer(auth.reads);
    emit(" write="); integer(auth.writes); emit(" errors="); integer(auth.errors); emit("\n");
    return auth.state == ChargeDone ? 0 : 1;
}
extern "C" int charge_main(long argc, char **argv) {
    char root[512] = {}; const char *output = nullptr; bool run = false; uint32_t wait = 0, wait_pd = 0;
    if (argc == 2 && same(argv[1],"--arm-fastboot")) return arm_fastboot();
    for (long i = 1; i < argc; ++i) {
        if (same(argv[i],"--authenticate")) run = true;
        else if (same(argv[i],"--sysfs") && i+1 < argc) {if (!copy(root,sizeof root,argv[++i])) return 2;}
        else if (same(argv[i],"--out") && i+1 < argc) output = argv[++i];
        else if (same(argv[i],"--wait") && i+1 < argc) {if (!number(argv[++i],10,&wait) || wait > 120) return 2;}
        else if (same(argv[i],"--wait-pd") && i+1 < argc) {if (!number(argv[++i],10,&wait_pd) || wait_pd > 120) return 2;}
        else {emit("usage: sheng-charge --arm-fastboot | [--authenticate] [--sysfs DIR] [--out FILE] [--wait SECONDS] [--wait-pd SECONDS]\n"); return same(argv[i],"--help") ? 0 : 2;}
    }
    if (output) {log_fd = (int)syscall(56,-100,(long)output,577,0600); if (log_fd < 0) return 2;}
    const uint64_t end = now_ms() + wait*1000u;
    while (!root[0] && !find_root(root)) {
        if (now_ms() >= end) {emit("[charge error] Xiaomi battery sysfs absent; load qcom_battmgr and ADSP dependencies/firmware\n"); return 2;}
        sleep_ms(100);
    }
    emit("[charge sysfs] "); emit(root); emit("\n"); telemetry(root);
    if (run && wait_pd) {
        emit("[charge auth] waiting for PD source\n");
        const uint64_t end_pd = now_ms() + wait_pd*1000u;
        while (source_type(root) < 6 && now_ms() < end_pd) sleep_ms(250);
    }
    const int result = run ? authenticate(root) : 0;
    if (run) telemetry(root);
    if (log_fd >= 0) {syscall(57,log_fd); log_fd = -1;}
    return result;
}
asm(".global _start\n_start:\nmov x0, sp\nldr x0, [x0]\nadd x1, sp, #8\nbl charge_main\nmov x8, #93\nsvc #0\nb .\n");
