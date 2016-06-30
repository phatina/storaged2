#!/usr/bin/python3

import os
import sys
import time
import subprocess
import argparse
import unittest
import storagedtestcase
import glob


VDEV_SIZE = 300000000  # size of virtual test device


def find_daemon(projdir):
    if os.path.exists(os.path.join(projdir, 'src', 'storaged')):
        daemon_bin = 'storaged'
    elif os.path.exists(os.path.join(projdir, 'src', 'udisksd')):
        daemon_bin = 'udisksd'
    else:
        print("Cannot find the daemon binary", file=sys.stderr)
        sys.exit(1)
    return daemon_bin


def setup_vdevs():
    '''create virtual test devices'''

    # craete 4 fake SCSI hard drives
    assert subprocess.call(['modprobe', 'scsi_debug', 'dev_size_mb=%i' % (
        VDEV_SIZE / 1048576), 'num_tgts=4']) == 0, 'Failure to modprobe scsi_debug'

    # wait until the drives got created
    dirs = []
    while len(dirs) < 4:
        dirs = glob.glob('/sys/bus/pseudo/drivers/scsi_debug/adapter*/host*/target*/*:*/block')
        time.sleep(0.1)
    assert len(dirs) == 4

    vdevs = []
    for d in dirs:
        devs = os.listdir(d)
        assert len(devs) == 1
        vdevs.append('/dev/' + devs[0])
        assert os.path.exists(vdevs[-1])

    # let's be 100% sure that we pick a virtual one
    for d in vdevs:
        assert open('/sys/block/%s/device/model' %
                    os.path.basename(d)).read().strip() == 'scsi_debug'

    storagedtestcase.test_devs = vdevs


if __name__ == '__main__':
    suite = unittest.TestSuite()
    daemon_log = sys.stdout

    argparser = argparse.ArgumentParser(description='storaged D-Bus test suite')
    argparser.add_argument('-l', '--log-file', dest='logfile',
                           help='write daemon log to a file')
    argparser.add_argument('testname', nargs='*',
                           help='name of test class or method (e. g. "Drive", "FS.test_ext2")')
    args = argparser.parse_args()

    # ensure that the scsi_debug module is loaded
    if os.path.isdir('/sys/module/scsi_debug'):
        sys.stderr.write('The scsi_debug module is already loaded; please '
                         'remove before running this test.\n')
        sys.exit(1)
    setup_vdevs()

    if args.logfile:
        daemon_log = open(args.logfile, mode='w')

    # find which binary we're about to test: this also affects the D-Bus interface and object paths
    testdir = os.path.abspath(os.path.dirname(__file__))
    projdir = os.path.abspath(os.path.normpath(os.path.join(testdir, '..', '..', '..')))
    daemon_bin = find_daemon(projdir)
    storagedtestcase.daemon_bin = daemon_bin
    daemon_bin_path = os.path.join(projdir, 'src', daemon_bin)

    # start the devel tree daemon
    daemon = subprocess.Popen([daemon_bin_path, '--replace', '--uninstalled',
        '--force-load-modules'], shell=False, stdout=daemon_log, stderr=daemon_log)
    # give the daemon some time to initialize
    time.sleep(3)
    daemon.poll()
    if daemon.returncode != None:
        print("Fatal: Unable to start the daemon process", file=sys.stderr)
        sys.exit(1)

    # Load all files in this directory whose name starts with 'test'
    if args.testname:
        for n in args.testname:
            suite.addTests(unittest.TestLoader().loadTestsFromName(n))
    else:
        for test_cases in unittest.defaultTestLoader.discover(testdir):
            suite.addTest(test_cases)
    result = unittest.TextTestRunner(verbosity=2).run(suite)

    daemon.terminate()
    daemon.wait()
    daemon_log.close()

    subprocess.call(['modprobe', '-r', 'scsi_debug'])

    if result.wasSuccessful():
        sys.exit(0)
    else:
        sys.exit(1)
