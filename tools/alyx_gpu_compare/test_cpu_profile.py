# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import io
import unittest

import cpu_profile

# Two samples in the shape xctrace exports: the second refers back to the first's thread,
# process, weight and frames by id.
XML = b'''<?xml version="1.0"?>
<trace-query-result><node><schema name="time-profile">
<col><mnemonic>time</mnemonic></col><col><mnemonic>thread</mnemonic></col>
<col><mnemonic>process</mnemonic></col><col><mnemonic>thread-state</mnemonic></col>
<col><mnemonic>weight</mnemonic></col><col><mnemonic>stack</mnemonic></col></schema>
<row><sample-time id="1">1000</sample-time>
<thread id="2" fmt="RenderThread (0x10) (hlvr.exe, pid: 42)"><tid id="3">16</tid></thread>
<process id="4" fmt="hlvr.exe (42)"/><thread-state id="5" fmt="Running">Running</thread-state>
<weight id="6" fmt="1.00 ms">1000000</weight>
<tagged-backtrace id="7"><frame id="8" name="0x180001000" addr="0x180001000"/>
<frame id="9" name="encode" addr="0x200000010"><binary id="10" name="winemetal.so"/></frame></tagged-backtrace></row>
<row><sample-time id="11">2001000</sample-time><thread ref="2"/><process ref="4"/><thread-state ref="5"/>
<weight ref="6"/><tagged-backtrace id="12"><frame ref="9"/></tagged-backtrace></row>
</node></trace-query-result>'''


class CpuProfileTest(unittest.TestCase):
    def test_rows_resolve_refs(self):
        rows = list(cpu_profile.time_profile_rows(io.BytesIO(XML)))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[1][1], 'RenderThread (0x10) (hlvr.exe, pid: 42)')
        self.assertEqual(rows[1][4], 1000000)
        self.assertEqual(rows[1][5], (('encode', 'winemetal.so', '0x200000010'),))
        self.assertEqual(rows[0][5][0], ('0x180001000', None, '0x180001000'))

    def test_profile_names_pe_code_from_the_map(self):
        mapper = cpu_profile.Mapper([(0x180000000, 0x180100000, 'd3d11.dll')])
        p = cpu_profile.profile(io.BytesIO(XML), (), mapper)
        key = ('hlvr.exe (42)', 'RenderThread (0x10)')
        self.assertEqual(p['per_thread'][key], 2000000)
        self.assertEqual(p['binaries'][key]['d3d11.dll (no symbols)'], 1000000)
        self.assertEqual(p['binaries'][key]['winemetal.so'], 1000000)
        self.assertIn('under encode [winemetal.so] [d3d11.dll (no symbols)]', p['functions'][key])
        self.assertAlmostEqual(p['span_s'], 0.002)

    def test_unmapped_and_system_addresses(self):
        mapper = cpu_profile.Mapper([])
        self.assertEqual(cpu_profile.leaf((('0x7ff812345678', None, '0x7ff812345678'),), mapper)[0],
                         '(system library, no symbols)')
        self.assertEqual(cpu_profile.leaf((('0x1234', None, '0x1234'),), mapper)[0], '(unknown)')
        self.assertEqual(cpu_profile.leaf((), mapper)[0], '(no stack)')

    def test_read_vmmap(self):
        import tempfile
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write('__TEXT                      180000000-180100000    [ 1024K] r-x/rwx SM=COW  '
                    '/Users/x/prefix/drive_c/windows/system32/d3d11.dll\n'
                    'MALLOC_TINY                 600000000-600100000    [ 1024K] rw-/rwx SM=PRV\n')
        self.assertEqual(cpu_profile.read_vmmap(f.name), [(0x180000000, 0x180100000, 'd3d11.dll'),
                                                          (0x600000000, 0x600100000, 'MALLOC_TINY')])

    def test_pe_modules_from_mapped_pieces(self):
        import struct
        import tempfile
        # a PE file: headers (0x400), sections .text at 0x1000 (0x3000) and .rdata at 0x4000 (0x2000)
        head = bytearray(0x400)
        head[0:2] = b'MZ'
        struct.pack_into('<I', head, 0x3c, 0x80)
        head[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<H', head, 0x86, 2)          # NumberOfSections
        struct.pack_into('<H', head, 0x94, 0xf0)       # SizeOfOptionalHeader
        struct.pack_into('<II', head, 0x98 + 56, 0x6000, 0x400)  # SizeOfImage, SizeOfHeaders
        for i, (va, size) in enumerate(((0x1000, 0x3000), (0x4000, 0x2000))):
            struct.pack_into('<III', head, 0x98 + 0xf0 + 40 * i + 8, size, va, size)
        d = tempfile.mkdtemp()
        dll = f'{d}/game.dll'
        with open(dll, 'wb') as f:
            f.write(head)
        with open(f'{d}/vm.txt', 'w') as f:
            # Wine maps only .rdata from the file; the image base is 0x6fff00000000
            f.write(f'mapped file                 6fff00004000-6fff00006000    [    8K] r--/rwx SM=COW  {dll}\n')
        self.assertEqual(cpu_profile.pe_modules(f'{d}/vm.txt'), [(0x6fff00000000, 0x6fff00006000, 'game.dll')])

    def test_thread_label(self):
        self.assertEqual(cpu_profile.thread_label('Main Thread (0x4d) (wineserver, pid: 3)'), 'Main Thread (0x4d)')


if __name__ == '__main__':
    unittest.main()
