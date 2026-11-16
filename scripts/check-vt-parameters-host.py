#!/usr/bin/env python3
"""Run the production VideoToolbox configuration parser on an HEVC MP4."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    args = parser.parse_args()
    image = args.fixture.read_bytes()
    at = image.find(b'hvcC')
    if at < 4:
        parser.error('fixture has no hvcC atom')
    length = int.from_bytes(image[at-4:at], 'big')
    if length < 31 or at-4+length > len(image):
        parser.error('fixture has a truncated hvcC atom')
    source = (ROOT / 'drivers/QcomIris/kernel/VtHost.cpp').read_text()
    function = source[source.index('bool parameterSets('):source.index('// --- Messages to the guest')]
    limit = re.search(r'constexpr uint32_t OutputBytes = [^;]+;', source)[0]
    harness = r'''
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include "VtWire.h"
using namespace vtwire;
static void *IOMalloc(size_t n) { return malloc(n); }
static void IOFree(void *p, size_t) { free(p); }
struct Session {
 bool tenBit=false;
 uint8_t temporalLayers=0;
 uint32_t nalLength=0;
 uint8_t *parameterSets=nullptr;
 uint32_t parameterSetsSize=0,parameterSetsCapacity=0;
 ~Session() { if(parameterSets)IOFree(parameterSets,parameterSetsCapacity); }
};
'''
    harness += limit + '\n' + function + r'''
int main(int argc,char **argv) {
 if(argc!=2)return 2;
 std::ifstream input(argv[1],std::ios::binary);
 std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
 ImageDescription d{};d.extensions=bytes.data();d.extensionsSize=bytes.size();
 Session s;
 if(!parameterSets(&s,d))return 3;
 printf("HEVC configuration: atom=%u AnnexB=%u capacity=%u\n",d.extensionsSize,s.parameterSetsSize,s.parameterSetsCapacity);
 bytes.pop_back();put32(bytes.data(),bytes.size());d.extensionsSize=bytes.size();
 if(parameterSets(&s,d))return 4;
 puts("PASS: complete configuration accepted; truncated NAL rejected");
}
'''
    with tempfile.TemporaryDirectory(prefix='vt-parameters-') as directory:
        work = Path(directory)
        (work / 'check.cpp').write_text(harness)
        (work / 'hvcc.bin').write_bytes(image[at-4:at-4+length])
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I', str(ROOT / 'drivers/QcomIris/kernel'), str(work / 'check.cpp'),
            '-o', str(work / 'check')], check=True)
        subprocess.run([str(work / 'check'), str(work / 'hvcc.bin')], check=True)


if __name__ == '__main__':
    main()
