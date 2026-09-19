import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]

class ReleaseImageTests(unittest.TestCase):
    def check_image(self, version, valid=True):
        workflow = yaml.safe_load((ROOT / '.github/workflows/firmware-release.yml').read_text())
        steps = workflow['jobs']['build']['steps']
        gates = [(i, s) for i, s in enumerate(steps) if s['name'] == 'Check release image version']
        self.assertEqual(len(gates), 1, 'Release builds must check the compiled version before exporting artifacts')
        index, step = gates[0]
        self.assertEqual(step['if'], "github.ref_type == 'tag'")
        self.assertLess(index, next(i for i,s in enumerate(steps) if s['name'] == 'Collect artifacts'))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            shutil.copytree(ROOT / 'scripts', root / 'scripts')
            (root / 'build').mkdir()
            data=bytearray(288)
            data[0]=0xe9
            struct.pack_into('<I',data,32,0xabcd5432 if valid else 0)
            data[48:48+len(version)]=version
            (root / 'build/mitsubishi-cn105-homekit.bin').write_bytes(data)
            return subprocess.run(['bash','-eo','pipefail','-c',step['run']],cwd=root,
                                  env=dict(os.environ,RELEASE_TAG='v0.2.6-beta.2'),capture_output=True,text=True)

    def test_release_command_survives_action_shell_wrapper(self):
        # espressif/esp-idf-ci-action@v1 puts command inside single quotes.
        # Exercise that real quoting boundary, with only idf.py replaced.
        workflow = yaml.safe_load((ROOT / '.github/workflows/firmware-release.yml').read_text())
        step = next(s for s in workflow['jobs']['build']['steps'] if s.get('uses', '').startswith('espressif/esp-idf-ci-action@'))
        for target, board, profile in [('esp32c6', 'nanoc6', 'NANOC6'), ('esp32s3', 'm5atoms3-lite', 'M5ATOMS3_LITE')]:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                shutil.copytree(ROOT / 'scripts', root / 'scripts')
                (root / 'test/project_ver').mkdir(parents=True)
                (root / 'test/project_ver/run.sh').write_text('exit 0\n')
                (root / '.release-tag').write_text('v0.2.6-beta.3')
                bindir = root / 'bin'
                bindir.mkdir()
                idf = bindir / 'idf.py'
                idf.write_text('#!/usr/bin/env python3\nimport json,os,sys\nwith open(os.environ["IDF_CALLS"],"a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n')
                idf.chmod(0o755)
                command = step['with']['command'].replace('${{ matrix.target }}', target).replace('${{ matrix.name }}', board)
                wrapper = "/bin/bash -c 'git config --global --add safe.directory \"*\" && " + command + "'"
                calls = root / 'calls.jsonl'
                result = subprocess.run(['bash', '-c', wrapper], cwd=root, capture_output=True, text=True,
                                        env=dict(os.environ, PATH=str(bindir)+os.pathsep+os.environ['PATH'],
                                                 GIT_CONFIG_GLOBAL=str(root/'gitconfig'), IDF_CALLS=str(calls)))
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertEqual([json.loads(line) for line in calls.read_text().splitlines()],
                                 [['set-target',target], ['-DRELEASE_TAG=v0.2.6-beta.3','-DBOARD_PROFILE_'+profile+'=1','build']])

    def test_exact_release_version_passes(self):
        result=self.check_image(b'v0.2.6-beta.2')
        self.assertEqual(result.returncode,0,result.stderr)

    def test_dirty_and_wrong_versions_stop_artifact_export(self):
        for version in [b'v0.2.6-beta.2-dirty',b'v0.2.6-beta.1',b'0.0.0-dev',b'x'*32]:
            with self.subTest(version=version):
                result=self.check_image(version)
                self.assertNotEqual(result.returncode,0)

    def test_missing_app_descriptor_stops_artifact_export(self):
        result=self.check_image(b'v0.2.6-beta.2',valid=False)
        self.assertNotEqual(result.returncode,0)

if __name__=='__main__':
    unittest.main(verbosity=2)
