import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('metal_tbdr',Path(__file__).resolve().parents[2]/'tools/analyze_metal_tbdr_trace.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Tests(unittest.TestCase):
    def test_streaming_references_keep_numeric_and_formatted_values(self):
        xml='''<trace-query-result><schema><col><mnemonic>duration</mnemonic></col><col><mnemonic>process</mnemonic></col></schema><row><duration id="1" fmt="2 ns">2</duration><process id="2" fmt="EXD (42)"/></row><row><duration ref="1"/><process ref="2"/></row></trace-query-result>'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'table.xml';p.write_text(xml);rows=list(module.rows(p))
        self.assertEqual(rows,[{'duration':(2,'2 ns'),'process':('EXD (42)','EXD (42)')}]*2)

    def test_nested_identifiers_and_missing_reference(self):
        xml='''<root><schema><col><mnemonic>encoder-id</mnemonic></col><col><mnemonic>label</mnemonic></col></schema><row><metal-encoder-id id="8">123</metal-encoder-id><string ref="missing"/></row><row><metal-encoder-id ref="8"/><string>R scene</string></row></root>'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'table.xml';p.write_text(xml);rows=list(module.rows(p))
        self.assertEqual(rows[0]['label'],(None,''))
        self.assertEqual(rows[1]['encoder-id'][0],123)

if __name__=='__main__':unittest.main()
