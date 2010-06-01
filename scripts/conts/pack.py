#! /usr/bin/env python2.6
# -*- mode: python; coding: utf-8; -*-
#
#  Codezero -- a microkernel for embedded systems.
#
#  Copyright © 2009  B Labs Ltd
#
import os, sys, shelve, glob
from os.path import join

PROJRELROOT = '../../'
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), PROJRELROOT)))
sys.path.append(os.path.abspath('../'))

from scripts.config.projpaths import *
from scripts.config.configuration import *

container_assembler_body = \
'''
.align 4
.section .img.%d
.incbin "%s"
'''

container_lds_start = \
'''/*
 * Autogenerated linker script that embeds each image to be
 * placed in a single container.
 *
 * Copyright (C) 2009 B Labs
 */

SECTIONS
{'''

container_lds_body = \
'''
	.img.%d : { *(.img.%d) }'''

container_lds_end = \
'''
}
'''

# Create container build base as:
# conts/linux -> build/cont[0-9]
def source_to_builddir(srcdir, id):
    cont_builddir = \
        os.path.relpath(srcdir, \
                        PROJROOT).replace("conts", \
                                          "cont" + str(id))
    return join(BUILDDIR, cont_builddir)

class LinuxContainerPacker:
    def __init__(self, container, linux_builder):

        # Here, we simply attempt to get PROJROOT/conts as
        # PROJROOT/build/cont[0-9]
        self.CONTAINER_BUILDDIR_BASE = \
            source_to_builddir(join(PROJROOT,'conts'), container.id)

        self.container_lds_out = join(self.CONTAINER_BUILDDIR_BASE, \
                                      'container.lds')
        self.container_S_out = join(self.CONTAINER_BUILDDIR_BASE, 'container.S')
        self.container_elf_out = join(self.CONTAINER_BUILDDIR_BASE, \
                                      'container' + str(container.id) + '.elf')

        self.kernel_image_in = linux_builder.kernel_image
        self.rootfs_elf_in = join(self.CONTAINER_BUILDDIR_BASE, 'linux/rootfs/rootfs.elf')
        self.atags_elf_in = join(self.CONTAINER_BUILDDIR_BASE, 'linux/atags/atags.elf')

    def generate_container_assembler(self, source):
        with open(self.container_S_out, 'w+') as f:
            file_body = ""
            img_i = 0
            for img in source:
                file_body += container_assembler_body % (img_i, img)
                img_i += 1

            f.write(file_body)
            f.close()

    def generate_container_lds(self, source):
        with open(self.container_lds_out, 'w+') as f:
            img_i = 0
            file_body = container_lds_start
            for img in source:
                file_body += container_lds_body % (img_i, img_i)
                img_i += 1
            file_body += container_lds_end
            f.write(file_body)
            f.close()

    def pack_container(self, config):
        self.generate_container_lds([self.kernel_image_in, self.rootfs_elf_in, \
                                     self.atags_elf_in])
        self.generate_container_assembler([self.kernel_image_in, self.rootfs_elf_in, \
                                           self.atags_elf_in])
        os.system(config.toolchain_kernel + "gcc " + "-nostdlib -o %s -T%s %s" \
                  % (self.container_elf_out, self.container_lds_out, \
                     self.container_S_out))
        # Final file is returned so that the final packer needn't
        # get the packer object for this information
        return self.container_elf_out

    def clean(self):
        os.system('rm -rf ' + self.container_elf_out)
        os.system('rm -rf ' + self.container_lds_out)
        os.system('rm -rf ' + self.container_S_out)


class DefaultContainerPacker:
    def __init__(self, container, images_in):

        # Here, we simply attempt to get PROJROOT/conts as
        # PROJROOT/build/cont[0-9]
        self.CONTAINER_BUILDDIR_BASE = \
            join(source_to_builddir(join(PROJROOT,'conts'), container.id), 'packer')

        if not os.path.exists(self.CONTAINER_BUILDDIR_BASE):
            os.mkdir(self.CONTAINER_BUILDDIR_BASE)

        self.container_lds_out = join(self.CONTAINER_BUILDDIR_BASE, \
                                      'container.lds')
        self.container_S_out = join(self.CONTAINER_BUILDDIR_BASE, 'container.S')
        self.container_elf_out = join(self.CONTAINER_BUILDDIR_BASE, \
                                      'container' + str(container.id) + '.elf')
        self.images_in = images_in

    def generate_container_assembler(self, source):
        with open(self.container_S_out, 'w+') as f:
            file_body = ""
            img_i = 0
            for img in source:
                file_body += container_assembler_body % (img_i, img)
                img_i += 1

            f.write(file_body)
            f.close()

    def generate_container_lds(self, source):
        with open(self.container_lds_out, 'w+') as f:
            img_i = 0
            file_body = container_lds_start
            for img in source:
                file_body += container_lds_body % (img_i, img_i)
                img_i += 1
            file_body += container_lds_end
            f.write(file_body)
            f.close()

    def pack_container(self, config):
        self.generate_container_lds(self.images_in)
        self.generate_container_assembler(self.images_in)
        os.system(config.toolchain_kernel + "gcc " + "-nostdlib -o %s -T%s %s" \
                  % (self.container_elf_out, self.container_lds_out, \
                     self.container_S_out))
        # Final file is returned so that the final packer needn't
        # get the packer object for this information
        return self.container_elf_out

    def clean(self):
        os.system('rm -f ' + self.container_elf_out)
        os.system('rm -f ' + self.container_lds_out)
        os.system('rm -f ' + self.container_S_out)

