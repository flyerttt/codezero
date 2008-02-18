#!/usr/bin/python
import os
import sys
from string import atoi
from os import popen2
from os.path import join

symbols = ['break_virtual']
builddir = "build"
loaderdir = "loader"
image = "start.axf"
imgpath = join(builddir, image)
asmfile = join(loaderdir, image) + ".S"
symfile = "ksyms"
asmheader = \
'''
/*
 * %s autogenerated from %s
 *
 * This file is included by the loader sources so that any
 * kernel symbol address can be known in advance and stopped
 * at by debuggers before virtual memory is enabled.
 */
'''

asmcontent = \
'''

.section .text
.align 4
.global %s;
.type	%s, function;
.equ	%s, %s

'''
def virt_to_phys(addr):
	return hex(int(addr, 16) - 0xF0000000)[:-1]

def ksym_to_lds():
	asm = open(asmfile, "w+")
	asm.write(asmheader % (asmfile, imgpath))
	cmd = "arm-none-eabi-objdump -d " + imgpath + " | grep " + "\\<" + symbols[0] + "\\>" + " > " + symfile
	os.system(cmd)
	kf = open(symfile, "r")
	#child_out, child_in = popen2(cmd)
	while True:
		line = kf.readline()
		if len(line) is 0:
			break
		addr, sym = line.split()
		sym = sym[1:-2]
		addr = "0x" + addr
		addr = virt_to_phys(addr)
	if sym in symbols:
		print "Adding " + sym + " from " + imgpath + " to " + asmfile + " in physical"
		asm.write(asmcontent % (sym, sym, sym, addr))
	asm.close()
	kf.close()
	cmd = "rm -rf " + symfile
	os.system(cmd)
if __name__ == "__main__":
	ksym_to_lds()
