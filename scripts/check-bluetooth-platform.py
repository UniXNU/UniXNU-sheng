#!/usr/bin/env python3
"""Exercise the production SM8550 clock and DMA binding with a register model."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1];soc=root.parent/'UniXNU-sm8550-common/kits/Sm8550Platform';smmu=root.parent/'UniXNU/kits/ArmSmmu4K'
out=root/'drivers/QcaBluetooth/out/platform-test';out.mkdir(parents=True,exist_ok=True)
sources=[soc/'Sm8550GeniUart.c',soc/'Sm8550QupDma.c',smmu/'ArmSmmu4K.c',root/'package/tests/bluetooth_platform.c']
subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(soc),'-I'+str(smmu),*map(str,sources),'-o',str(out/'platform')],check=True)
r=subprocess.run([out/'platform'],text=True,capture_output=True);(out/'output.txt').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.stdout+r.stderr)
(out/'report.json').write_text(json.dumps(dict(passed=True,registers_simulated=True,hardware_verified=False,
    inputs={str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},output=r.stdout),indent=2)+'\n')
print(r.stdout,end='')
