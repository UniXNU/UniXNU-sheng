#!/usr/bin/env python3
"""Run V3.1 synthetic touch contracts against actual board and central repositories.

Requires Python3.10+, a C11/C++17 host compiler and ASan/UBSan. No downloads,
device operations, Apple SDK, kernel, firmware or sensor capture is used.
Place --out outside both repositories; reports redact repository/work paths.
"""
from pathlib import Path
import argparse,datetime,hashlib,json,os,platform,re,runpy,shutil,subprocess,sys

PACKAGE=Path(__file__).resolve().parents[1]
ASSETS=PACKAGE/"tests/touch-v3p1"
FIXTURES=ASSETS/"fixtures"
EXPECTED_GROUPS={
 "test_astra_core.cpp":22,"test_astra_coverage.cpp":22,"test_astra_engine.cpp":7,
 "test_astra_formula_config.cpp":11,"test_astra_nuisance_counterexample.cpp":1,
 "test_astra_partial.cpp":17,"test_astra_probe.cpp":8,"test_astra_quantization.cpp":1,
 "test_astra_region.cpp":18,"test_astra_timeline.cpp":15}
def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def require(ok,message):
 if not ok: raise RuntimeError(message)

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument("--board-root",type=Path,default=PACKAGE,help="Board checkout with drivers/ and package/; defaults to this repository")
 ap.add_argument("--unixnu-root",type=Path,default=PACKAGE.parent/"UniXNU",help="Companion central UniXNU checkout for the pinned builder")
 ap.add_argument("--out",type=Path,required=True)
 ap.add_argument("--report-dir",type=Path,help="Optional sanitized text/JSON report destination")
 ap.add_argument("--legacy-author-core",action="store_true",help="Also execute the unchanged author core fixture; failure is reported separately")
 args=ap.parse_args()
 source=args.board_root.resolve();unixnu=args.unixnu_root.resolve();out=args.out.resolve()
 def source_path(name):
  if name.startswith("device/"): return source/name[len("device/"):]
  if name.startswith("UniXNU/"): return unixnu/name[len("UniXNU/"):]
  raise RuntimeError("Unexpected source namespace: "+name)
 require(not any(out.is_relative_to(p) for p in (PACKAGE,source,unixnu)),"Build products must stay outside both repositories")
 out.mkdir(parents=True,exist_ok=False)
 report_dir=args.report_dir.resolve() if args.report_dir else out/"sanitized-reports"
 report_dir.mkdir(parents=True,exist_ok=True)
 pins=json.loads((ASSETS/"tools/expected-production-hashes.json").read_text(encoding="utf-8"))
 require(len(pins)==19,"Expected exact nineteen-file integrated-source binding")
 actual={p:digest(source_path(p)) for p in pins}
 require(actual==pins,"Production source differs from the reviewed integrated V3.1 source profile")
 fixture_provenance=json.loads((ASSETS/"FIXTURE-PROVENANCE.json").read_text(encoding="utf-8"))
 for name,item in fixture_provenance["files"].items():
  require(digest(ASSETS/name)==item["copied_sha256"],"Fixture differs: "+name)
 require({p.name for p in FIXTURES.glob("test_astra_*.cpp")}==set(EXPECTED_GROUPS),"Unexpected Astra fixture set")
 cc=shutil.which(os.environ.get("CC","cc"));cxx=shutil.which(os.environ.get("CXX","c++"))
 require(cc and cxx,"Host C/C++ compiler with sanitizers required")
 flags=["-g","-O1","-Wall","-Wextra","-Werror","-Wno-misleading-indentation",
        "-fsanitize=address,undefined","-fno-omit-frame-pointer"]
 env=dict(os.environ,TMPDIR=str(out),TMP=str(out),TEMP=str(out),
          PYTHONDONTWRITEBYTECODE="1",ASAN_OPTIONS="halt_on_error=1",
          UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
 env.pop("NVT_FIRMWARE",None)
 commands=[];groups=[];extra=[];raw_outputs={}
 def sanitized(text):
  for path,name in [(source,"<BOARD>"),(unixnu,"<UNIXNU>"),(out,"<WORK>"),(PACKAGE,"<TEST-REPO>")]:
   text=text.replace(str(path),name)
  return text
 def public_command(cmd):
  cmd=[sanitized(str(s)) for s in cmd]
  return [Path(s).name if i==0 and s in (cc,cxx,sys.executable) else s for i,s in enumerate(cmd)]
 def run(cmd,name,required=True):
  cmd=list(map(str,cmd));commands.append(public_command(cmd))
  p=subprocess.run(cmd,capture_output=True,text=True,encoding="utf-8",errors="replace",env=env)
  (out/(name+".stdout")).write_text(p.stdout,encoding="utf-8")
  (out/(name+".stderr")).write_text(p.stderr,encoding="utf-8")
  raw_outputs[name]=p
  if required:
   require(p.returncode==0,"Command failed: "+name+"\n"+sanitized(p.stdout+p.stderr))
   require(not re.search(r"runtime error:|ERROR: (?:AddressSanitizer|LeakSanitizer)|UndefinedBehaviorSanitizer|Sanitizer:DEADLYSIGNAL",p.stderr),
           "Sanitizer failure: "+name+"\n"+sanitized(p.stderr))
  return p
 d=source/"drivers/ShengNovatekTouch";pkg=source/"package";hid=source/"drivers/ShengHidInput"
 run([sys.executable,"-B",ASSETS/"tools/extract_astra_probe.py","--probe",pkg/"TouchProbe.cpp","--out",out],"probe-extraction")
 objects=[]
 for stem in ["NvtProtocol","NvtTouchCore","NvtPartialObservation","NvtCoverage"]:
  obj=out/(stem+".o")
  run([cc,"-std=c11",*flags,"-Wframe-larger-than=8192","-I",d,"-MMD","-MF",out/(stem+".d"),"-c",d/(stem+".c"),"-o",obj],stem+"-compile")
  objects.append(obj)
 for filename,count in EXPECTED_GROUPS.items():
  fixture=FIXTURES/filename;name=fixture.stem;binary=out/name
  run([cxx,"-std=c++17",*flags,"-I",d,"-I",pkg,"-I",out,fixture,*objects,"-o",binary],name+"-compile")
  p=run([binary],name)
  names=[s[5:] for s in p.stdout.splitlines() if s.startswith("PASS ")]
  require(len(names)==count,"Named case count differs in "+filename)
  rows=[json.loads(s) for s in p.stdout.splitlines() if s.startswith("{")]
  if rows: require(rows[-1].get("tests",count)==count,"Declared case count differs in "+filename)
  item={"fixture":filename,"status":"PASS_HOST_SYNTHETIC_NOT_HARDWARE","named_checks":count,"checks":names,"rows":rows}
  groups.append(item)
  (report_dir/(name+".stdout.txt")).write_text(sanitized(p.stdout),encoding="utf-8")
  print(json.dumps({"suite":name,"named_checks":count,"status":"PASS"}),flush=True)
 require(sum(x["named_checks"] for x in groups)==122,"Astra aggregate differs from122")
 # Compile and run the unchanged public chip model without optional private firmware.
 protocol=d/"Test/nvt-protocol.c"
 if protocol.exists():
  binary=out/"protocol-model"
  run([cc,"-std=c11",*flags,"-I",d,protocol,objects[0],"-o",binary],"protocol-model-compile")
  p=run([binary],"protocol-model")
  require("real-firmware tests skipped" in p.stdout,"Protocol model unexpectedly entered a private-firmware path")
  extra.append({"suite":"public_protocol_chip_model","status":"PASS_SYNTHETIC_MODEL_ONLY",
                "firmware_file_used":False,"counted_in_122":False,"output":sanitized(p.stdout)})
 # Use the actual V3.1 sender body with explicit platform stubs.
 function=runpy.run_path(str(ASSETS/"tools/extract_astra_probe.py"))["function"]
 sender=(hid/"NativeHidInput.cpp").read_text(encoding="utf-8")
 body=function(sender,"    void touchSend()")
 barrier='__asm__ volatile("dmb ish":::"memory");'
 require(body.count(barrier)==2 and body.count("NativeHidInput *self=this;")==1,"Current sender extraction differs")
 host_body=body.replace("NativeHidInput *self=this;","NewDevice *self=this;").replace(barrier,"host_barrier();")
 template=(FIXTURES/"hid/extracted_touchsend.template.cpp").read_text(encoding="utf-8")
 require(template.count("/* ACTUAL_V3P1_TOUCHSEND_BODY */")==1,"Missing exact sender insertion point")
 legacy=function((FIXTURES/"hid/touchSend.baseline.cpp.txt").read_text(encoding="utf-8"),"    void touchSend()")
 old_body=function(template[template.index("struct OldDevice {"):],"    void touchSend()")
 require(legacy.replace("NativeHidInput *self=this;","OldDevice *self=this;").replace(barrier,"host_barrier();")==old_body,
         "Stored old-sender control differs from pinned public original")
 generated=template.replace("/* ACTUAL_V3P1_TOUCHSEND_BODY */",host_body)
 (out/"extracted_touchsend.cpp").write_text(generated,encoding="utf-8")
 binary=out/"HID-sender"
 run([cc,"-std=c11",*flags,"-I",hid,"-c",FIXTURES/"hid/pure_c_compile.c","-o",out/"pure-c-hid.o"],"pure-c-hid-compile")
 run([cxx,"-std=c++17",*flags,"-I",hid,out/"extracted_touchsend.cpp","-o",binary],"HID-sender-compile")
 p=run([binary],"HID-sender")
 names=[s[5:] for s in p.stdout.splitlines() if s.startswith("PASS ")]
 require(len(names)==15,"Expected old14HIDcases plus originaltime/ACK regression")
 rows=[json.loads(s) for s in p.stdout.splitlines() if s.startswith("{")]
 require(rows[-1]["tests"]==15 and rows[-1]["report_bytes_including_ID"]==62,"HID summary differs")
 nested=[s for s in rows if s.get("kind")=="Core12_set_property"]
 require(len(nested)==1 and nested[0]["cases"]==9936,"HID generated set coverage differs")
 extra.append({"suite":"actual_V3p1_HID_sender_with_platform_stubs","status":"PASS_HOST_SYNTHETIC_NOT_HARDWARE",
               "named_checks":15,"counted_in_122":False,"generated_set_transitions":9936,"checks":names,
               "source_sha256":actual["device/drivers/ShengHidInput/NativeHidInput.cpp"],
               "generated_harness_sha256":hashlib.sha256(generated.encode()).hexdigest(),
               "permitted_substitutions":["NativeHidInput->NewDevice self type","two ARM DMB instructions->host memory barriers"],
               "platform_dependencies_stubbed":["IOBufferMemoryDescriptor allocation","HID submission result/time","clock/report globals","HID driver availability"],
               "pure_C_header_compile":"PASS","output":sanitized(p.stdout)})
 # A prior author test may assert the old scan-count lifecycle. Never retune the
 # production algorithm to make such a test pass; record its outcome independently.
 if args.legacy_author_core:
  author_pin=json.loads((ASSETS/"tools/expected-author-fixture.json").read_text(encoding="utf-8"))
  fixture_equal=(d/"Test/touch-core.c").exists() and digest(d/"Test/touch-core.c")==author_pin["sha256"]
  require(fixture_equal,"The checked-out author fixture differs from its pinned public SHA256")
  author_fixtures=[
   ("author_baseline_core_fixture",d/"Test/touch-core.c","69fdd6223ed2572aa8bd8351e77c53a238276130")]
  for label,fixture,revision in author_fixtures:
   if not fixture.exists():
    extra.append({"suite":label,"status":"NOT_RUN_NOT_EXPORTED","counted_in_122":False,"fixture_revision":revision})
    continue
   # The author's fixture uses ../NvtTouchCore.h. Stage the unchanged fixture
   # beside V3.1 headers, never the latest-author struct definitions by accident.
   stage=out/label; (stage/"Test").mkdir(parents=True)
   for header in d.glob("*.h"): shutil.copyfile(header,stage/header.name)
   staged=stage/"Test/touch-core.c";shutil.copyfile(fixture,staged)
   require(digest(staged)==digest(fixture),"Author fixture changed during staging")
   binary=stage/"core-fixture-check"
   built=run([cc,"-std=c11",*flags,"-I",d,staged,objects[1],"-lm","-o",binary],label+"-compile",False)
   p=run([binary],label,False) if built.returncode==0 else built
   extra.append({"suite":label,"status":"PASS_HOST_AUTHOR_FIXTURE" if p.returncode==0 else "FAIL_COMPATIBILITY_REVIEW_REQUIRED",
                 "counted_in_122":False,"compile_returncode":built.returncode,"returncode":p.returncode,
                 "fixture_revision":revision,
                 "baseline_fixture_byte_identical":fixture_equal,"fixture_sha256":digest(fixture),
                 "header_sha256":actual["device/drivers/ShengNovatekTouch/NvtTouchCore.h"],
                 "output":sanitized(p.stdout+p.stderr),"production_source_modified_to_pass":False})
 require({p:digest(source_path(p)) for p in pins}==pins,"Production source changed during checks")
 extraction=json.loads((out/"probe-extraction.json").read_text(encoding="utf-8"))
 report={"schema":"unixnu.touch-contribution.host-results.v1",
         "status":"PASS_122_HOST_SYNTHETIC_WITH_SEPARATE_EXTRA_RESULTS_NOT_HARDWARE",
         "source_scope":"V3.1 algorithms in the actual board checkout and companion central UniXNU checkout; nineteen pinned changed-file hashes include two build scripts. Host checks only, no native kernel/device qualification.",
         "source_namespaces":{"device/":"<BOARD>/","UniXNU/":"<UNIXNU>/"},
         "executed_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),"astra_groups":10,"astra_named_checks":122,
         "groups":groups,"extra_suites":extra,
         "environment":{"host_system":platform.system(),"host_machine":platform.machine(),
                        "cc":run([cc,"--version"],"cc-version").stdout.splitlines()[0],
                        "cxx":run([cxx,"--version"],"cxx-version").stdout.splitlines()[0],
                        "python":platform.python_version(),"sanitizers":["address","undefined"]},
         "production_source_hashes":actual,"fixture_provenance_sha256":digest(ASSETS/"FIXTURE-PROVENANCE.json"),
         "probe_extraction":extraction,"commands":commands,
         "real_sensor_files_used":False,"device_identity_used":False,"private_firmware_used":False,
         "native_iOS_kernel_built":False,"hardware_tested":False,
         "limits":["Host fixtures validate modeled source behavior, not physical sensor coverage or user experience.",
                   "The integrated author SPI/SMMU coordination outside extracted Probe helpers and the two build scripts are hash-bound but not dynamically qualified by this host suite.",
                   "The HIDsink return in the harness is a simulated API acknowledgement, not OS event-consumer delivery.",
                   "ARM barriers are host barriers only; target ABI/timing and actual iOS handleReportWithTime are not qualified here.",
                   "The optional legacy author core outcome is separate; inspect extra_suites rather than inferring it from the122pass status."]}
 (report_dir/"host-results.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
 print(json.dumps({"status":report["status"],"astra_named_checks":122,
                   "extra_suites":[{"suite":x["suite"],"status":x["status"]} for x in extra]}),flush=True)
if __name__=="__main__":
 try: main()
 except (RuntimeError,subprocess.SubprocessError) as error:
  print("ERROR: "+str(error),file=sys.stderr);raise SystemExit(1)
