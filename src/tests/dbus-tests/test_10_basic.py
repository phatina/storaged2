import udiskstestcase
import dbus
import os
import six
import shutil

from config_h import UDISKS_MODULES_ENABLED

class UdisksBaseTest(udiskstestcase.UdisksTestCase):
    '''This is a base test suite'''

    # A map between module name (ID) and corresponding org.freedesktop.UDisks2.Manager interface
    UDISKS_MODULE_MANAGER_IFACES = {'btrfs': 'BTRFS',
                                    'iscsi': 'ISCSI.Initiator',
                                    'lvm2': 'LVM2'}

    def setUp(self):
        self.manager_obj = self.get_object('/Manager')

    def _get_modules(self):
        # The 'lsm' module is intentionally not tested here to avoid its initialization
        # at this point. It's tested thoroughly in test_19_lsm.
        return UDISKS_MODULES_ENABLED - {'lsm'}

    def _get_udisks2_conf_path(self):
        _CONF_FILE = 'udisks2.conf'
        if os.environ['UDISKS_TESTS_ARG_SYSTEM'] == '1':
            return os.path.join('/etc/udisks2/', _CONF_FILE)
        else:
            return os.path.join(os.environ['UDISKS_TESTS_PROJDIR'], 'udisks', _CONF_FILE)

    def test_10_manager(self):
        '''Testing the manager object presence'''
        self.assertIsNotNone(self.manager_obj)
        version = self.get_property(self.manager_obj, '.Manager', 'Version')
        version.assertIsNotNone()

    def _restore_udisks2_conf(self):
        if self.udisks2_conf_contents:
            self.write_file(self._get_udisks2_conf_path(), self.udisks2_conf_contents)
        else:
            self.remove_file(self._get_udisks2_conf_path(), ignore_nonexistent=True)

    def test_20_enable_modules(self):
        manager = self.get_interface(self.manager_obj, '.Manager')

        # make a backup of udisks2.conf
        self.udisks2_conf_contents = None
        try:
            self.udisks2_conf_contents = self.read_file(self._get_udisks2_conf_path())
        except FileNotFoundError as e:
            # no existing udisks2.conf, simply remove the file once finished
            pass

        # mock the udisks2.conf to load only tested modules
        modules = self._get_modules()
        modules.add('inva/id')
        contents = '[udisks2]\nmodules=%s\n' % ','.join(modules)
        self.write_file(self._get_udisks2_conf_path(), contents)
        self.addCleanup(self._restore_udisks2_conf)

        manager.EnableModules(dbus.Boolean(True))

        manager_intro = dbus.Interface(self.manager_obj, "org.freedesktop.DBus.Introspectable")
        intro_data = manager_intro.Introspect()
        for module in self._get_modules():
            self.assertIn('interface name="%s.Manager.%s"' % (self.iface_prefix, self.UDISKS_MODULE_MANAGER_IFACES[module]), intro_data)

    def test_21_enable_single_module(self):
        manager = self.get_interface(self.manager_obj, '.Manager')
        # Test that no error is returned for already loaded modules
        for module in self._get_modules():
            with self.assertRaises(dbus.exceptions.DBusException):
                manager.EnableModule(module, dbus.Boolean(False))
            manager.EnableModule(module, dbus.Boolean(True))
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException,
                                   r'cannot open shared object file: No such file or directory'):
            manager.EnableModule("non-exist_ent", dbus.Boolean(True))
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException,
                                   r'Module unloading is not currently supported.'):
            manager.EnableModule("nonexistent", dbus.Boolean(False))
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException,
                                   r'Requested module name .* is not a valid udisks2 module name.'):
            manager.EnableModule("inváálěd", dbus.Boolean(True))
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException,
                                   r'Requested module name .* is not a valid udisks2 module name.'):
            manager.EnableModule("inváálěd", dbus.Boolean(False))
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException,
                                   r'Requested module name .* is not a valid udisks2 module name.'):
            manager.EnableModule("module/../intruder", dbus.Boolean(True))

    def test_30_supported_filesystems(self):
        fss = self.get_property(self.manager_obj, '.Manager', 'SupportedFilesystems')
        self.assertEqual({str(s) for s in fss.value},
                         {'nilfs2', 'btrfs', 'swap', 'ext2', 'ext3', 'ext4', 'udf', 'xfs', 'f2fs', 'ntfs', 'vfat', 'exfat'})

    def test_40_can_format(self):
        '''Test for installed filesystem creation utility with CanFormat'''
        manager = self.get_interface(self.manager_obj, '.Manager')
        with self.assertRaises(dbus.exceptions.DBusException):
            manager.CanFormat('wxyz')
        for fs in ('xfs', 'f2fs', 'ext4'):
            avail, util = manager.CanFormat(fs)
            if avail:
                self.assertEqual(util, '')
            else:
                self.assertEqual(util, 'mkfs.%s' % fs)
            self.assertEqual(avail, shutil.which('mkfs.%s' % fs) is not None)
        for fs in map(str, self.get_property(self.manager_obj, '.Manager', 'SupportedFilesystems').value):
            avail, util = manager.CanFormat(fs)
            # currently UDisks relies on executables for filesystem creation
            if avail:
                self.assertEqual(util, '')
            else:
                self.assertGreater(len(util), 0)

    def test_40_can_resize(self):
        '''Test for installed filesystem resize utility with CanResize'''
        offline_shrink = 0b00010
        offline_grow   = 0b00100
        online_shrink  = 0b01000
        online_grow    = 0b10000
        manager = self.get_interface(self.manager_obj, '.Manager')
        with self.assertRaises(dbus.exceptions.DBusException):
            manager.CanResize('minix')
        avail, mode, util = manager.CanResize('xfs')
        # the resize mode flags values are defined in the method documentation
        self.assertEqual(mode, online_grow | offline_grow)
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'xfs_growfs')
        self.assertEqual(avail, shutil.which('xfs_growfs') is not None)
        avail, mode, util = manager.CanResize('ext4')
        self.assertEqual(mode, offline_shrink | offline_grow | online_grow)
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'resize2fs')
        self.assertEqual(avail, shutil.which('resize2fs') is not None)
        avail, mode, util = manager.CanResize('vfat')
        # the only valid reason for VFAT not being available is missing vfat-resize
        # the support is either compiled in (libblockdev 2.x) or via vfat-resize (3.x)
        if not avail:
            self.assertEqual(util, 'vfat-resize')

        if shutil.which('vfat-resize') is not None:
            self.assertTrue(avail)

    def test_40_can_repair(self):
        '''Test for installed filesystem repair utility with CanRepair'''
        manager = self.get_interface(self.manager_obj, '.Manager')
        with self.assertRaises(dbus.exceptions.DBusException):
            manager.CanRepair('nilfs2')
        avail, util = manager.CanRepair('xfs')
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'xfs_repair')
        self.assertEqual(avail, shutil.which('xfs_repair') is not None)
        avail, util = manager.CanRepair('ext4')
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'e2fsck')
        self.assertEqual(avail, shutil.which('e2fsck') is not None)
        avail, util = manager.CanRepair('vfat')
        self.assertTrue(avail)  # libparted, no executable
        self.assertEqual(util, '')

    def test_40_can_check(self):
        '''Test for installed filesystem check utility with CanCheck'''
        manager = self.get_interface(self.manager_obj, '.Manager')
        with self.assertRaises(dbus.exceptions.DBusException):
            manager.CanCheck('nilfs2')
        avail, util = manager.CanCheck('xfs')
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'xfs_db')
        self.assertEqual(avail, shutil.which('xfs_db') is not None)
        avail, util = manager.CanCheck('ext4')
        if avail:
            self.assertEqual(util, '')
        else:
            self.assertEqual(util, 'e2fsck')
        self.assertEqual(avail, shutil.which('e2fsck') is not None)
        avail, util = manager.CanCheck('vfat')
        self.assertTrue(avail)  # libparted, no executable
        self.assertEqual(util, '')

    def test_50_get_block_devices(self):
        # get all objects and filter block devices
        udisks = self.get_object('')
        objects = udisks.GetManagedObjects(dbus_interface='org.freedesktop.DBus.ObjectManager')
        block_paths = [p for p in list(objects.keys()) if "/block_devices/" in p]

        # get block devices using the 'GetBlockDevices' function
        manager = self.get_interface(self.manager_obj, '.Manager')
        dbus_blocks = manager.GetBlockDevices(self.no_options)

        # and make sure both lists are equal
        self.assertEqual(len(block_paths), len(dbus_blocks))
        for path in block_paths:
            self.assertIn(path, dbus_blocks)

    def _wipe(self, device, retry=True):
        ret, out = self.run_command('wipefs -a %s' % device)
        if ret != 0:
            if retry:
                self._wipe(device, False)
            else:
                self.fail('Failed to wipe device %s: %s' % (device, out))

    def test_60_resolve_device(self):
        manager = self.get_interface(self.manager_obj, '.Manager')

        # empty/invalid devspec supplied
        spec = dbus.Dictionary({}, signature='sv')
        msg = r'Invalid device specification provided'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            manager.ResolveDevice(spec, self.no_options)
        spec = dbus.Dictionary({'PATH': '/dev/i-dont-exist'}, signature='sv')
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            manager.ResolveDevice(spec, self.no_options)

        # try some non-existing device first
        spec = dbus.Dictionary({'path': '/dev/i-dont-exist'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        self.assertEqual(len(devices), 0)
        spec = dbus.Dictionary({'uuid': 'I-DONT-EXIST'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        self.assertEqual(len(devices), 0)
        spec = dbus.Dictionary({'label': 'I-DONT-EXIST'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        self.assertEqual(len(devices), 0)
        spec = dbus.Dictionary({'partuuid': 'I-DONT-EXIST'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        self.assertEqual(len(devices), 0)
        spec = dbus.Dictionary({'partlabel': 'I-DONT-EXIST'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        self.assertEqual(len(devices), 0)

        # get our first virtual disk by path
        spec = dbus.Dictionary({'path': self.vdevs[0]}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[0]))

        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)

        # try to get it with some symlink path
        ldir = '/dev/disk/by-id'
        link = next(os.path.join(ldir, l) for l in os.listdir(ldir)
                    if os.path.realpath(os.path.join(ldir, l)) == self.vdevs[0])

        spec = dbus.Dictionary({'path': link}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[0]))

        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)

        # try to get the disk by specifying both path and label (it has no label
        # so this should return an empty list)
        spec = dbus.Dictionary({'path': self.vdevs[0], 'label': 'test'}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)

        self.assertEqual(len(devices), 0)

        # format the disk to ext4 with label
        label = 'test'
        ret, out = self.run_command('mkfs.ext4 -F -L %s %s' % (label, self.vdevs[0]))
        if ret != 0:
            self.fail('Failed to create ext4 filesystem on %s: %s' % (self.vdevs[0], out))
        self.addCleanup(self._wipe, self.vdevs[0])

        # we run mkfs manually, now just wait for the property to update
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        dbus_label = self.get_property(disk, '.Block', 'IdLabel')
        dbus_label.assertEqual(label)

        # now it should return the disk
        spec = dbus.Dictionary({'path': self.vdevs[0], 'label': label}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[0]))

        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)

        # format another disk to ext4 with the same label, ResolveDevice should
        # now return both devices
        ret, _out = self.run_command('mkfs.ext4 -F -L %s %s' % (label, self.vdevs[1]))
        if ret != 0:
            self.fail('Failed to create ext4 filesystem on %s: %s' % (self.vdevs[1], out))
        self.addCleanup(self._wipe, self.vdevs[1])

        # we run mkfs manually, now just wait for the property to update
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[1]))
        dbus_label = self.get_property(disk, '.Block', 'IdLabel')
        dbus_label.assertEqual(label)

        spec = dbus.Dictionary({'label': label}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path1 = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[0]))
        object_path2 = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[1]))

        self.assertEqual(len(devices), 2)
        self.assertIn(object_path1, devices)
        self.assertIn(object_path2, devices)

        # and now just check that simple resolving using uuid works
        uuid = self.get_property_raw(disk, '.Block', 'IdUUID')
        spec = dbus.Dictionary({'uuid': uuid}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s' % (self.path_prefix, os.path.basename(self.vdevs[1]))

        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)

        # create a partition on another device
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[2]))
        self.assertIsNotNone(disk)
        disk.Format('gpt', self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.addCleanup(self._wipe, self.vdevs[2])
        part_label = 'PRTLBLX'
        path = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2),
                                    '', part_label, self.no_options,
                                    dbus_interface=self.iface_prefix + '.PartitionTable')
        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)
        part_uuid = self.get_property_raw(part, '.Partition', 'UUID')
        self.assertIsNotNone(part_uuid)
        part_name_val = self.get_property_raw(part, '.Partition', 'Name')
        self.assertEquals(part_name_val, part_label)

        # check that partlabel and partuuid can be resolved
        spec = dbus.Dictionary({'partlabel': part_label}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s1' % (self.path_prefix, os.path.basename(self.vdevs[2]))
        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)
        spec = dbus.Dictionary({'partuuid': part_uuid}, signature='sv')
        devices = manager.ResolveDevice(spec, self.no_options)
        object_path = '%s/block_devices/%s1' % (self.path_prefix, os.path.basename(self.vdevs[2]))
        self.assertEqual(len(devices), 1)
        self.assertIn(object_path, devices)

    def test_80_device_presence(self):
        '''Test the debug devices are present on the bus'''
        for d in self.vdevs:
            dev_obj = self.get_object("/block_devices/%s" % os.path.basename(d))
            self.assertIsNotNone(dev_obj)
            self.assertTrue(os.path.exists(d))
