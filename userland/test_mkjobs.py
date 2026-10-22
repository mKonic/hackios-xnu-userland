"""Actual mkjobs.py recipe, synthetic plists; no Apple files or guest needed."""
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RECIPE = ROOT / 'userland/mkjobs.py'


class JobsTests(unittest.TestCase):
    def run_recipe(self, packed):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'root'
            jobs = root / 'System/Library/LaunchDaemons'
            angels = root / 'System/Library/LaunchAngels'
            jobs.mkdir(parents=True)
            angels.mkdir()
            (root / 'usr/libexec').mkdir(parents=True)
            labels = ['com.test.primary', 'com.test.alpha', 'com.test.beta', 'com.test.angel']
            for i, label in enumerate(labels):
                program = '/usr/libexec/test' + str(i)
                (root / program[1:]).write_bytes(b'fixture')
                job = dict(Label=label, Program=program, MachServices={label: True},
                           RunAtLoad=True, KeepAlive=True, StartInterval=30)
                folder = angels if label.endswith('.angel') else jobs
                (folder / (label + '.plist')).write_bytes(plistlib.dumps(job))
            dest = Path(tmp) / 'output'
            env = os.environ.copy()
            env.pop('RWA_PACKED16', None)
            env.pop('ALLOW', None)
            if packed:
                env['RWA_PACKED16'] = '1'
            subprocess.run([sys.executable, str(RECIPE), '--rest',
                            str(root), str(dest), labels[0]], env=env, check=True,
                           capture_output=True)
            self.assertEqual({p.name for p in dest.iterdir()}, {s + '.plist' for s in labels})
            for label in labels:
                job = plistlib.loads((dest / (label + '.plist')).read_bytes())
                self.assertEqual(job['Label'], label)
                if label != labels[0]:
                    for key in ('RunAtLoad', 'KeepAlive', 'StartInterval'):
                        self.assertNotIn(key, job)
                self.assertEqual(job.get('EnvironmentVariables', {}).get('RWA_PACKED16'),
                                 '1' if packed else None)

    def test_rest_files_without_experiment(self):
        self.run_recipe(False)

    def test_rest_files_with_experiment(self):
        self.run_recipe(True)


if __name__ == '__main__':
    unittest.main()
