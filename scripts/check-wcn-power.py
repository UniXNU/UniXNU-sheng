#!/usr/bin/env python3
"""Check production WCN ownership against Wi-Fi/Bluetooth coexistence sequences."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'package/WifiProbe.cpp').read_text()
body=source[source.index('IOLock *gWcnLock;'):source.index('// The MEM window')]
out=root/'drivers/QcaBluetooth/out/power-test';out.mkdir(parents=True,exist_ok=True)
prefix=r'''
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include "WcnPower.h"
using IOLock=std::mutex;
static IOLock *IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock *l){delete l;}
static void IOLockLock(IOLock *l){l->lock();}
static void IOLockUnlock(IOLock *l){l->unlock();}
static unsigned native_wifi_pmic[32],prepare,enable,disable,pcie;
static bool rail_failure,disable_failure,pmic_failure;
static void pmicPrepare(){prepare++;native_wifi_pmic[10]=pmic_failure?3:0xff;}
static int powerRails(bool shared,bool bus,bool on=true){
 if(shared && on)enable++;
 if(shared && !on)disable++;
 if(bus)pcie++;
 return (shared && on && rail_failure) || (shared && !on && disable_failure)?-1:0;
}
'''
tests=r'''
int main(){
 assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));assert(enable==1 && !pcie && prepare==1);
 assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));assert(enable==1);
 assert(!sheng_wcn_power_get(SHENG_WCN_WLAN));assert(enable==1 && pcie==1);
 assert(sheng_wcn_power_owners()==3);
 assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));assert(!disable && sheng_wcn_power_owners()==1);
 assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));assert(enable==1 && prepare==1);
 assert(!sheng_wcn_power_put(SHENG_WCN_WLAN));assert(!disable && sheng_wcn_power_owners()==2);
 disable_failure=true;assert(sheng_wcn_power_put(SHENG_WCN_BLUETOOTH)==EIO);assert(sheng_wcn_power_owners()==2);
 disable_failure=false;assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));assert(sheng_wcn_power_owners()==0);
 assert(!sheng_wcn_power_get(SHENG_WCN_WLAN));assert(enable==2 && pcie==2 && prepare==2);
 assert(!sheng_wcn_power_get(SHENG_WCN_BLUETOOTH));assert(enable==2 && prepare==2);
 unsigned before=disable;assert(!sheng_wcn_power_put(SHENG_WCN_WLAN));assert(disable==before);
 assert(!sheng_wcn_power_put(SHENG_WCN_BLUETOOTH));assert(disable==before+1);
 rail_failure=true;assert(sheng_wcn_power_get(SHENG_WCN_BLUETOOTH)==EIO);assert(!sheng_wcn_power_owners());
 rail_failure=false;pmic_failure=true;before=enable;
 assert(sheng_wcn_power_get(SHENG_WCN_BLUETOOTH)==EIO && enable==before && !sheng_wcn_power_owners());
 assert(sheng_wcn_power_get(2)==EINVAL && sheng_wcn_power_put(2)==EINVAL);
 delete gWcnLock;
}
'''
(out/'power.cpp').write_text(prefix+body+tests)
subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'package'),str(out/'power.cpp'),'-o',str(out/'power')],check=True)
subprocess.run([out/'power'],check=True)
(out/'report.json').write_text(json.dumps(dict(passed=True,hardware_verified=False,source_sha256=hashlib.sha256(body.encode()).hexdigest()),indent=2)+'\n')
print('PASS: independent radio votes, shared supply lifetime, Bluetooth-only PCIe exclusion and power failures')
