#!/usr/bin/env python3
"""QEMU's virt device tree with the SM6115's CPU idle topology
(freebsd/pm/test/qemu/run.sh), after KolibriOS's tests/power/mkdtb.py:

    mkdtb.py OUT.dtb [--smp N] [--mem 2G]

QEMU's own tree (dumped by QEMU: GICv3, a Cortex-A53) plus the phone's
hierarchy (arm64/qcom/sm6115.dtsi): each CPU in a power domain of its own
with the core's rail power collapse, the first half of the CPUs below
cluster 0's domain (GDHS and power collapse), the second half below
cluster 1's. The domains are not below /psci, which QEMU replaces in a
tree it is given. The suspend parameters are in PSCI 0.2's original
format, the only one QEMU's CPU_SUSPEND takes (bits 31-17 zero; bit 16
power-down, a 16-bit state id), with the phone's latencies and
residencies; local-timer-stop as on the phone (in QEMU the timer keeps
running through its CPU_SUSPEND, a wfi; hw.psci_idle.simulate plays the
loss). Needs dtc.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--smp', type=int, default=8)
ap.add_argument('--mem', default='2G')
a = ap.parse_args()

with tempfile.TemporaryDirectory() as tmp:
    raw = os.path.join(tmp, 'virt.dtb')
    subprocess.run(['qemu-system-aarch64', '-M', f'virt,gic-version=3,dumpdtb={raw}',
                    '-cpu', 'cortex-a53', '-smp', str(a.smp), '-m', a.mem, '-display', 'none'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', raw], check=True,
                         capture_output=True, text=True).stdout

half = max(1, a.smp // 2)
states = '''
		idle-states {
			entry-method = "psci";

			little_cpu_sleep_0: cpu-sleep-0-0 {
				compatible = "arm,idle-state";
				idle-state-name = "silver-rail-power-collapse";
				arm,psci-suspend-param = <0x10003>;
				entry-latency-us = <290>;
				exit-latency-us = <376>;
				min-residency-us = <1182>;
				local-timer-stop;
			};

			big_cpu_sleep_0: cpu-sleep-1-0 {
				compatible = "arm,idle-state";
				idle-state-name = "gold-rail-power-collapse";
				arm,psci-suspend-param = <0x10003>;
				entry-latency-us = <297>;
				exit-latency-us = <324>;
				min-residency-us = <1110>;
				local-timer-stop;
			};
		};

		domain-idle-states {
			cluster_0_sleep_0: cluster-sleep-0-0 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10022>;
				entry-latency-us = <360>;
				exit-latency-us = <421>;
				min-residency-us = <782>;
			};

			cluster_0_sleep_1: cluster-sleep-0-1 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10044>;
				entry-latency-us = <800>;
				exit-latency-us = <2118>;
				min-residency-us = <7376>;
			};

			cluster_1_sleep_0: cluster-sleep-1-0 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10042>;
				entry-latency-us = <314>;
				exit-latency-us = <345>;
				min-residency-us = <660>;
			};

			cluster_1_sleep_1: cluster-sleep-1-1 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10044>;
				entry-latency-us = <640>;
				exit-latency-us = <1654>;
				min-residency-us = <8094>;
			};
		};
'''
m = re.search(r'\n\tcpus \{\n', dts)
if not m:
    sys.exit('mkdtb: no /cpus in QEMU\'s tree')
# before the first subnode of /cpus (its properties come first)
n = re.compile(r'\t\t[^\t\n]+ \{\n').search(dts, m.end())
dts = dts[:n.start()] + states.lstrip('\n') + '\n' + dts[n.start():]

for cpu in range(a.smp):
    big = cpu >= half
    pat = re.compile(r'(\t\tcpu@%x \{\n)' % cpu)
    if not pat.search(dts):
        sys.exit(f'mkdtb: no cpu@{cpu:x}')
    dts = pat.sub(lambda mm: mm.group(1) +
                  f'\t\t\tpower-domains = <&cpu_pd{cpu}>;\n\t\t\tpower-domain-names = "psci";\n',
                  dts, count=1)

pds = '\n\tpower-domains-psci {\n'
for cpu in range(a.smp):
    big = cpu >= half
    pds += f'''		cpu_pd{cpu}: power-domain-cpu{cpu} {{
			#power-domain-cells = <0>;
			power-domains = <&cluster_{1 if big else 0}_pd>;
			domain-idle-states = <&{"big" if big else "little"}_cpu_sleep_0>;
		}};

'''
pds += '''		cluster_0_pd: power-domain-cpu-cluster0 {
			#power-domain-cells = <0>;
			domain-idle-states = <&cluster_0_sleep_0>, <&cluster_0_sleep_1>;
		};

		cluster_1_pd: power-domain-cpu-cluster1 {
			#power-domain-cells = <0>;
			domain-idle-states = <&cluster_1_sleep_0>, <&cluster_1_sleep_1>;
		};
	};
'''
i = dts.rindex('};')
dts = dts[:i] + pds + dts[i:]

with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, 'pm.dts')
    with open(src, 'w') as f:
        f.write(dts)
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', a.out, src], check=True)
print(f'mkdtb: {a.out} ({a.smp} CPUs, clusters of {half})')
