import dbus
import os
import six
import shutil
import re
import tempfile
import unittest
import time

from bytesize import bytesize
from collections import namedtuple
from contextlib import contextmanager
from packaging.version import Version

import udiskstestcase


Device = namedtuple('Device', ['obj', 'obj_path', 'path', 'name', 'size'])


class UdisksBtrfsTest(udiskstestcase.UdisksTestCase):
    '''This is a basic test suite for btrfs interface'''

    @classmethod
    def setUpClass(cls):
        udiskstestcase.UdisksTestCase.setUpClass()
        if not cls.check_module_loaded('btrfs'):
            udiskstestcase.UdisksTestCase.tearDownClass()
            raise unittest.SkipTest('Udisks module for btrfs tests not loaded, skipping.')

    @contextmanager
    def _temp_mount(self, device):
        tmp = tempfile.mkdtemp()
        self.run_command('mount %s %s' % (device, tmp))

        try:
            yield tmp
        finally:
            self.run_command('umount %s' % tmp)
            shutil.rmtree(tmp)

    def _clean_format(self, device):
        d = dbus.Dictionary(signature='sv')
        d['erase'] = True
        device.Format('empty', d, dbus_interface=self.iface_prefix + '.Block')

    def _get_devices(self, num_devices):
        devices = []

        for i in range(num_devices):
            dev_path = self.vdevs[i]
            dev_name = dev_path.split('/')[-1]

            dev_obj = self.get_object('/block_devices/' + dev_name)
            self.assertIsNotNone(dev_obj)

            _ret, out = self.run_command('lsblk -d -b -no SIZE %s' % dev_path)  # get size of the device

            dev = Device(dev_obj, dbus.ObjectPath(dev_obj.object_path), dev_path, dev_name, int(out))
            devices.append(dev)

        return devices

    def _get_btrfs_version(self):
        _ret, out = self.run_command('btrfs --version')
        m = re.search(r'[Bb]trfs.* v([\d\.]+)', out)
        if not m or len(m.groups()) != 1:
            raise RuntimeError('Failed to determine btrfs version from: %s' % out)
        return Version(m.groups()[0])

    def test__manager_interface(self):
        '''Test for module D-Bus Manager interface presence'''

        manager = self.get_object('/Manager')
        intro_data = manager.Introspect(self.no_options, dbus_interface='org.freedesktop.DBus.Introspectable')
        self.assertIn('interface name="%s.Manager.BTRFS"' % self.iface_prefix, intro_data)

    def test_create(self):
        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_single', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        # check filesystem type
        usage = self.get_property(dev.obj, '.Block', 'IdUsage')
        usage.assertEqual('filesystem')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        # check '.Filesystem.BTRFS' properties
        dbus_label = self.get_property(dev.obj, '.Filesystem.BTRFS', 'label')
        _ret, sys_label = self.run_command('lsblk -d -no LABEL %s' % dev.path)
        dbus_label.assertEqual('test_single')
        dbus_label.assertEqual(sys_label)

        dbus_uuid = self.get_property(dev.obj, '.Filesystem.BTRFS', 'uuid')
        _ret, sys_uuid = self.run_command('lsblk -d -no UUID %s' % dev.path)
        dbus_uuid.assertEqual(sys_uuid)

        dbus_devs = self.get_property(dev.obj, '.Filesystem.BTRFS', 'num_devices')
        dbus_devs.assertEqual(1)

        # check size
        with self._temp_mount(dev.path) as mnt:
            _ret, out = self.run_command('btrfs filesystem show %s' % mnt)
            m = re.search(r'.*devid +1 size (.+) used', out, flags=re.DOTALL)
            sys_size = bytesize.Size(m.group(1))
            self.assertEqual(sys_size.convert_to(bytesize.B), dev.size)

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSTABLE)
    def test_create_raid(self):
        devs = self._get_devices(2)

        for dev in devs:
            self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')

        # invalid raid level
        msg = '[uU]nknown profile raidN'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            manager.CreateVolume([dev.obj_path for dev in devs],
                                 'test_raidN', 'raidN', 'raidN',
                                 self.no_options,
                                 dbus_interface=self.iface_prefix + '.Manager.BTRFS')


        manager.CreateVolume([dev.obj_path for dev in devs],
                             'test_raid1', 'raid1', 'raid1',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        # check filesystem type
        for dev in devs:
            usage = self.get_property(dev.obj, '.Block', 'IdUsage')
            usage.assertEqual('filesystem')

            fstype = self.get_property(dev.obj, '.Block', 'IdType')
            fstype.assertEqual('btrfs')

            # check '.Filesystem.BTRFS' properties
            dbus_label = self.get_property(dev.obj, '.Filesystem.BTRFS', 'label')
            _ret, sys_label = self.run_command('lsblk -d -no LABEL %s' % dev.path)
            dbus_label.assertEqual('test_raid1')
            dbus_label.assertEqual(sys_label)

            dbus_uuid = self.get_property(dev.obj, '.Filesystem.BTRFS', 'uuid')
            _ret, sys_uuid = self.run_command('lsblk -d -no UUID %s' % dev.path)
            dbus_uuid.assertEqual(sys_uuid)

            dbus_devs = self.get_property(dev.obj, '.Filesystem.BTRFS', 'num_devices')
            dbus_devs.assertEqual(2)

            # check data and metadata raid level
            with self._temp_mount(dev.path) as mnt:
                _ret, out = self.run_command('btrfs filesystem df %s' % mnt)
                self.assertIn('Data, RAID1:', out)
                self.assertIn('Metadata, RAID1:', out)

    def test_subvolumes(self):

        btrfs_version = self._get_btrfs_version()
        if btrfs_version == Version('4.13.2'):
            self.skipTest('subvolumes list is broken with btrfs-progs v4.13.2')

        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_subvols', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        # not mounted, should fail
        msg = 'org.freedesktop.UDisks2.Error.NotMounted: Volume not mounted'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            dev.obj.CreateSubvolume('test_sub1', self.no_options,
                                    dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        with self._temp_mount(dev.path) as mnt:
            # create a subvolume
            dev.obj.CreateSubvolume('test_sub1', self.no_options,
                                    dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            _ret, out = self.run_command('btrfs subvolume list %s' % mnt)
            name = out.strip().split(' ')[-1]
            self.assertEqual(name, 'test_sub1')

            # list all subvolumes
            subs, num = dev.obj.GetSubvolumes(False, self.no_options,
                                              dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            self.assertEqual(num, 1)
            self.assertEqual(subs[0][2], 'test_sub1')  # name (path) is the third item

            # delete the subvolume
            dev.obj.RemoveSubvolume('test_sub1', self.no_options,
                                    dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            _ret, out = self.run_command('btrfs subvolume list %s' % mnt)
            self.assertFalse(out)  # no subvolume -> empty output

            # list all subvolumes
            _subs, num = dev.obj.GetSubvolumes(False, self.no_options,
                                               dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            self.assertEqual(num, 0)

    def test_add_remove_device(self):
        dev1, dev2 = self._get_devices(2)
        self.addCleanup(self._clean_format, dev1.obj)
        self.addCleanup(self._clean_format, dev2.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev1.obj_path],
                             'test_add_remove', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev1.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        # shouldn't be possible to remove only device
        with self._temp_mount(dev1.path):
            msg = 'unable to remove the only writeable device'
            with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
                dev1.obj.RemoveDevice(dev1.obj_path, self.no_options,
                                      dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # add second device to the volume
        with self._temp_mount(dev1.path):
            dev1.obj.AddDevice(dev2.obj_path, self.no_options,
                               dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # check filesystem type of the new device
        fstype = self.get_property(dev2.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        # check number of devices
        dbus_devs = self.get_property(dev1.obj, '.Filesystem.BTRFS', 'num_devices')
        dbus_devs.assertEqual(2)

        _ret, out = self.run_command('btrfs filesystem show %s' % dev1.path)
        self.assertIn('Total devices 2 FS', out)
        self.assertIn('path %s' % dev1.path, out)
        self.assertIn('path %s' % dev2.path, out)

        # remove the second device
        with self._temp_mount(dev1.path):
            dev1.obj.RemoveDevice(dev2.obj_path, self.no_options,
                                  dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            fstype = self.get_property(dev2.obj, '.Block', 'IdType')
            fstype.assertFalse()

        # check number of devices
        dbus_devs = self.get_property(dev1.obj, '.Filesystem.BTRFS', 'num_devices')
        dbus_devs.assertEqual(1)

        _ret, out = self.run_command('btrfs filesystem show %s' % dev1.path)
        self.assertIn('Total devices 1 FS', out)
        self.assertIn('path %s' % dev1.path, out)
        self.assertNotIn('path %s' % dev2.path, out)

    def test_snapshot(self):
        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_snapshot', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        with self._temp_mount(dev.path) as mnt:
            # create a subvolume
            dev.obj.CreateSubvolume('test_sub1', self.no_options,
                                    dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

            # create snapshot
            dev.obj.CreateSnapshot('test_sub1', 'test_sub1_snapshot', False, self.no_options,
                                   dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

            # list snapshots
            snaps, num = dev.obj.GetSubvolumes(True, self.no_options,
                                               dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')
            self.assertEqual(num, 1)
            self.assertEqual(snaps[0][2], 'test_sub1_snapshot')  # name (path) is the third item

            _ret, out = self.run_command('btrfs subvolume list %s' % mnt)
            self.assertIn('path test_sub1', out)
            self.assertIn('path test_sub1_snapshot', out)

    def test_resize(self):
        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_snapshot', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        with self._temp_mount(dev.path) as mnt:
            new_size = dev.size - 20 * 1024**2
            dev.obj.Resize(dbus.UInt64(new_size), self.no_options,
                           dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

            _ret, out = self.run_command('btrfs filesystem show %s' % mnt)
            m = re.search(r'.*devid +1 size (.+) used', out, flags=re.DOTALL)
            sys_size = bytesize.Size(m.group(1))
            self.assertEqual(sys_size.convert_to(bytesize.B), new_size)

    def test_label(self):
        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_label', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        dev.obj.SetLabel('new_label', self.no_options,
                         dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        dbus_label = self.get_property(dev.obj, '.Filesystem.BTRFS', 'label')
        dbus_label.assertEqual('new_label')

        _ret, sys_label = self.run_command('lsblk -d -no LABEL %s' % dev.path)
        self.assertEqual(sys_label, 'new_label')

    def test_subvolume_mount(self):
        dev = self._get_devices(1)[0]
        self.addCleanup(self._clean_format, dev.obj)

        manager = self.get_object('/Manager')
        manager.CreateVolume([dev.obj_path],
                             'test_snapshot', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(dev.obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')

        with self._temp_mount(dev.path):
            # create a subvolume
            dev.obj.CreateSubvolume('test_sub1', self.no_options,
                                    dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        self.addCleanup(self.try_unmount, dev.path)
        opts = dbus.Dictionary({'options': 'subvol=/test_sub1'},
                               signature=dbus.Signature('sv'))
        mnt_path = dev.obj.Mount(opts, dbus_interface=self.iface_prefix + '.Filesystem')

        # system mountpoint
        self.assertTrue(os.path.ismount(mnt_path))
        _ret, out = self.run_command('mount | grep %s' % dev.path)
        self.assertIn(mnt_path, out)
        self.assertIn('subvol=/test_sub1', out)

        # dbus mountpoint
        dbus_mounts = self.get_property(dev.obj, '.Filesystem', 'MountPoints')
        dbus_mounts.assertLen(1)  # just one mountpoint
        dbus_mnt = self.ay_to_str(dbus_mounts.value[0])  # mountpoints are arrays of bytes
        self.assertEqual(dbus_mnt, mnt_path)

    def test_multivolume(self):
        devs = self._get_devices(4)
        for i_dev in devs:
            self.addCleanup(self.wipe_fs, i_dev.path)

        manager = self.get_object('/Manager')
        manager.CreateVolume([devs[0].obj_path],
                             'test_multivolume', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        fstype = self.get_property(devs[0].obj, '.Block', 'IdType')
        fstype.assertEqual('btrfs')
        dbus_devs = self.get_property(devs[0].obj, '.Filesystem.BTRFS', 'num_devices')
        dbus_devs.assertEqual(1)

        # mount it as a single device
        mnt_path = devs[0].obj.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.addCleanup(self.try_unmount, mnt_path)
        self.assertIsNotNone(mnt_path)
        self.assertTrue(os.path.ismount(mnt_path))
        self.assertTrue('test_multivolume' in mnt_path)

        # save ID_FS_UUID for later
        fs_uuid = self.get_property_raw(devs[0].obj, '.Block', 'IdUUID')
        self.assertIsNotNone(fs_uuid)
        self.assertGreater(len(fs_uuid), 0)

        # verify that additional devices carry no signature
        for i_dev in devs[1:]:
            i_fstype = self.get_property(i_dev.obj, '.Block', 'IdType')
            i_fstype.assertEqual('')
            i_fs_uuid = self.get_property(i_dev.obj, '.Block', 'IdUUID')
            i_fs_uuid.assertEqual('')
            msg = 'No such interface .org.freedesktop.UDisks2.Filesystem.BTRFS'
            with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
                # no btrfs module interface should be attached
                self.get_property_raw(i_dev.obj, '.Filesystem.BTRFS', 'num_devices')

        # convert to a multidisk volume
        for i_dev in devs[1:]:
            devs[0].obj.AddDevice(i_dev.obj_path, self.no_options,
                                  dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # verify that properties on all disks are updated
        for i_dev in devs:
            dbus_devs = self.get_property(i_dev.obj, '.Filesystem.BTRFS', 'num_devices')
            dbus_devs.assertEqual(4)
            dbus_mounts = self.get_property(i_dev.obj, '.Filesystem', 'MountPoints')
            dbus_mounts.assertLen(1)  # just one mountpoint
            dbus_mnt = self.ay_to_str(dbus_mounts.value[0])  # mountpoints are arrays of bytes
            self.assertEqual(dbus_mnt, mnt_path)
            i_fstype = self.get_property(i_dev.obj, '.Block', 'IdType')
            i_fstype.assertEqual('btrfs')
            i_fs_uuid = self.get_property_raw(i_dev.obj, '.Block', 'IdUUID')
            self.assertEqual(i_fs_uuid, fs_uuid)
            dbus_label = self.get_property(i_dev.obj, '.Filesystem.BTRFS', 'label')
            dbus_label.assertEqual('test_multivolume')

        # unmount it
        devs[0].obj.Unmount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.assertFalse(os.path.ismount(mnt_path))
        self.assertFalse(os.path.exists(mnt_path))

        # change the label
        devs[0].obj.SetLabel('btr12345678fs', self.no_options,
                             dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # verify that mountpoint disappeared from all objects
        for i_dev in devs:
            dbus_mounts = self.get_property(i_dev.obj, '.Filesystem', 'MountPoints')
            dbus_mounts.assertLen(0)

        # mount again on a different block device
        mnt_path2 = devs[3].obj.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.addCleanup(self.try_unmount, mnt_path2)
        self.assertIsNotNone(mnt_path2)
        self.assertTrue(os.path.ismount(mnt_path2))
        self.assertTrue('btr12345678fs' in mnt_path2)

        # verify that properties on all disks are updated
        for i_dev in devs:
            dbus_mounts = self.get_property(i_dev.obj, '.Filesystem', 'MountPoints')
            dbus_mounts.assertLen(1)
            dbus_mnt = self.ay_to_str(dbus_mounts.value[0])
            self.assertEqual(dbus_mnt, mnt_path2)
            dbus_devs = self.get_property(i_dev.obj, '.Filesystem.BTRFS', 'num_devices')
            dbus_devs.assertEqual(4)
            dbus_label = self.get_property(i_dev.obj, '.Filesystem.BTRFS', 'label')
            dbus_label.assertEqual('btr12345678fs')

        # remove the devices
        for i_dev in devs[:3]:
            devs[3].obj.RemoveDevice(i_dev.obj_path, self.no_options,
                                     dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # check number of devices
        dbus_devs = self.get_property(devs[3].obj, '.Filesystem.BTRFS', 'num_devices')
        dbus_devs.assertEqual(1)

        # verify that additional devices carry no signature after removal
        for i_dev in devs[:3]:
            msg = 'No such interface .org.freedesktop.UDisks2.Filesystem'
            with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
                self.get_property_raw(i_dev.obj, '.Filesystem', 'MountPoints')
            msg = 'No such interface .org.freedesktop.UDisks2.Filesystem.BTRFS'
            with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
                self.get_property_raw(i_dev.obj, '.Filesystem.BTRFS', 'num_devices')
            i_fstype = self.get_property(i_dev.obj, '.Block', 'IdType')
            i_fstype.assertEqual('')
            i_fs_uuid = self.get_property(i_dev.obj, '.Block', 'IdUUID')
            i_fs_uuid.assertEqual('')

        # unmount and check that the mountpoint is gone
        devs[3].obj.Unmount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.assertFalse(os.path.ismount(mnt_path2))
        self.assertFalse(os.path.exists(mnt_path2))

    def test_multivolume_missing(self):
        devs = self._get_devices(4)
        for i_dev in devs:
            self.addCleanup(self.wipe_fs, i_dev.path)

        manager = self.get_object('/Manager')
        manager.CreateVolume([devs[0].obj_path],
                             'test_multivolume_missing', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        mnt_path = devs[0].obj.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.addCleanup(self.try_unmount, mnt_path)
        self.assertIsNotNone(mnt_path)
        self.assertTrue(os.path.ismount(mnt_path))

        for i_dev in devs[1:]:
            devs[0].obj.AddDevice(i_dev.obj_path, self.no_options,
                                  dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # unmount it
        devs[0].obj.Unmount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.assertFalse(os.path.ismount(mnt_path))
        self.assertFalse(os.path.exists(mnt_path))

        # wipe the second block device
        self.wipe_fs(devs[1].path)

        # try to mount it again with a missing component
        msg = 'Error mounting /dev/sd. at /.*: wrong fs type, bad option, bad superblock'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            mnt_path = devs[0].obj.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')

    def test_multivolume_external(self):
        devs = self._get_devices(4)
        for i_dev in devs:
            self.addCleanup(self.wipe_fs, i_dev.path)

        manager = self.get_object('/Manager')
        manager.CreateVolume([devs[0].obj_path],
                             'test_multivolume_external', 'single', 'single',
                             self.no_options,
                             dbus_interface=self.iface_prefix + '.Manager.BTRFS')

        mnt_path = devs[0].obj.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.addCleanup(self.try_unmount, mnt_path)
        self.assertIsNotNone(mnt_path)
        self.assertTrue(os.path.ismount(mnt_path))

        for i_dev in devs[1:]:
            devs[0].obj.AddDevice(i_dev.obj_path, self.no_options,
                                  dbus_interface=self.iface_prefix + '.Filesystem.BTRFS')

        # unmount it
        devs[0].obj.Unmount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.assertFalse(os.path.ismount(mnt_path))
        self.assertFalse(os.path.exists(mnt_path))

        # verify that mountpoint disappeared from all objects
        for i_dev in devs:
            dbus_mounts = self.get_property(i_dev.obj, '.Filesystem', 'MountPoints')
            dbus_mounts.assertLen(0)

        # mount it outside of UDisks
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        ret, _out = self.run_command('mount %s %s' % (devs[0].path, tmp))
        self.addCleanup(self.try_unmount, tmp)
        self.assertEqual(ret, 0)

        # give UDisks some time to register the event
        time.sleep(2)

        # verify that properties on all disks are updated
        for i_dev in devs:
            dbus_mounts = self.get_property(i_dev.obj, '.Filesystem', 'MountPoints')
            dbus_mounts.assertLen(1)
            dbus_mnt = self.ay_to_str(dbus_mounts.value[0])
            self.assertEqual(dbus_mnt, tmp)
