import copy
import dbus
import glob
import fcntl
import os
import time

import udiskstestcase


class UdisksBlockTest(udiskstestcase.UdisksTestCase):
    '''This is a basic block device test suite'''

    LUKS_PASSPHRASE = 'shouldnotseeme'

    def _close_luks(self, disk):
        disk.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')

    def test_format(self):

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # this might fail in case of stray udev rules or records in /etc/fstab
        hint_auto = self.get_property_raw(disk, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(False))
        hint_ignore = self.get_property_raw(disk, '.Block', 'HintIgnore')
        self.assertEqual(hint_ignore, dbus.Boolean(False))
        hint_partitionable = self.get_property_raw(disk, '.Block', 'HintPartitionable')
        self.assertEqual(hint_partitionable, dbus.Boolean(True))
        hint_system = self.get_property_raw(disk, '.Block', 'HintSystem')
        self.assertEqual(hint_system, dbus.Boolean(True))

        # create xfs filesystem
        disk.Format('xfs', self.no_options, dbus_interface=self.iface_prefix + '.Block')

        usage = self.get_property(disk, '.Block', 'IdUsage')
        usage.assertEqual('filesystem')

        fstype = self.get_property(disk, '.Block', 'IdType')
        fstype.assertEqual('xfs')

        hint_auto = self.get_property_raw(disk, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(False))
        hint_ignore = self.get_property_raw(disk, '.Block', 'HintIgnore')
        self.assertEqual(hint_ignore, dbus.Boolean(False))
        hint_partitionable = self.get_property_raw(disk, '.Block', 'HintPartitionable')
        self.assertEqual(hint_partitionable, dbus.Boolean(True))
        hint_system = self.get_property_raw(disk, '.Block', 'HintSystem')
        self.assertEqual(hint_system, dbus.Boolean(True))

        _ret, sys_fstype = self.run_command('lsblk -d -no FSTYPE %s' % self.vdevs[0])
        self.assertEqual(sys_fstype, 'xfs')

        # remove the format
        d = dbus.Dictionary(signature='sv')
        d['erase'] = True
        disk.Format('empty', d, dbus_interface=self.iface_prefix + '.Block')

        # check if the disk is empty
        usage = self.get_property(disk, '.Block', 'IdUsage')
        usage.assertEqual('')

        fstype = self.get_property(disk, '.Block', 'IdType')
        fstype.assertEqual('')

        hint_auto = self.get_property_raw(disk, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(False))
        hint_ignore = self.get_property_raw(disk, '.Block', 'HintIgnore')
        self.assertEqual(hint_ignore, dbus.Boolean(False))
        hint_partitionable = self.get_property_raw(disk, '.Block', 'HintPartitionable')
        self.assertEqual(hint_partitionable, dbus.Boolean(True))
        hint_system = self.get_property_raw(disk, '.Block', 'HintSystem')
        self.assertEqual(hint_system, dbus.Boolean(True))

        _ret, sys_fstype = self.run_command('lsblk -d -no FSTYPE %s' % self.vdevs[0])
        self.assertEqual(sys_fstype, '')

    def test_format_parttype(self):

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create partition table first
        disk.Format('dos', self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # now create partition
        path = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2),
                                    '', '', self.no_options,
                                    dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        # now format it to swap with 'update-partition-type'
        d = dbus.Dictionary(signature='sv')
        d['update-partition-type'] = True
        part.Format('swap', d, dbus_interface=self.iface_prefix + '.Block')

        # part type should be set to swap (0x42 or 0x82)
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertIn(['0x42', '0x82'])

        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertIn(sys_type, ['0x42', '0x82'])

    def test_open(self):

        # O_ACCMODE is node defined in Python 2 version of 'os' module
        try:
            from os import O_ACCMODE
        except ImportError:
            O_ACCMODE = 3

        # format the disk
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        disk.Format('xfs', self.no_options, dbus_interface=self.iface_prefix + '.Block')

        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # OpenForBackup
        dbus_fd = disk.OpenForBackup(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(dbus_fd)

        fd = dbus_fd.take()
        mode = fcntl.fcntl(fd, fcntl.F_GETFL) & O_ACCMODE
        self.assertEqual(mode, os.O_RDONLY)
        os.close(fd)

        # OpenForRestore
        dbus_fd = disk.OpenForRestore(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(dbus_fd)

        fd = dbus_fd.take()
        mode = fcntl.fcntl(fd, fcntl.F_GETFL) & O_ACCMODE
        self.assertEqual(mode, os.O_WRONLY)
        os.close(fd)

        # OpenForBenchmark
        dbus_fd = disk.OpenForBenchmark(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(dbus_fd)

        fd = dbus_fd.take()
        mode = fcntl.fcntl(fd, fcntl.F_GETFL)
        self.assertTrue(bool(mode & os.O_DIRECT))
        self.assertTrue(bool(mode & os.O_SYNC))
        os.close(fd)

        # OpenDevice
        with self.assertRaises(dbus.exceptions.DBusException):  # invalid mode
            disk.OpenDevice('abc', self.no_options, dbus_interface=self.iface_prefix + '.Block')

        d = dbus.Dictionary(signature='sv')
        d['flags'] = os.O_RDWR
        with self.assertRaises(dbus.exceptions.DBusException):  # read-only mode with O_RDWR flag
            disk.OpenDevice('r', d, dbus_interface=self.iface_prefix + '.Block')

        d = dbus.Dictionary(signature='sv')
        d['flags'] = os.O_ASYNC | os.O_DIRECT
        dbus_fd = disk.OpenDevice('w', d, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(dbus_fd)

        fd = dbus_fd.take()
        mode = fcntl.fcntl(fd, fcntl.F_GETFL)
        self.assertTrue(bool(mode & os.O_WRONLY))
        self.assertTrue(bool(mode & os.O_DIRECT))
        self.assertTrue(bool(mode & os.O_ASYNC))
        os.close(fd)

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSAFE)
    def test_configuration_fstab(self):

        # this test will change /etc/fstab, we might want to revert the changes when it finishes
        fstab = self.read_file('/etc/fstab')
        self.addCleanup(self.write_file, '/etc/fstab', fstab)

        # format the disk
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        disk.Format('xfs', self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # cleanup -- remove format
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # configuration items as arrays of dbus.Byte
        mnt = self.str_to_ay('/mnt/test')
        fstype = self.str_to_ay('xfs')
        opts = self.str_to_ay('defaults')

        # set the new configuration
        conf = dbus.Dictionary({'dir': mnt, 'type': fstype, 'opts': opts, 'freq': 0, 'passno': 0},
                               signature=dbus.Signature('sv'))
        disk.AddConfigurationItem(('fstab', conf), self.no_options,
                                  dbus_interface=self.iface_prefix + '.Block')

        # get the configuration
        old_conf = self.get_property(disk, '.Block', 'Configuration')
        old_conf.assertTrue()
        self.assertEqual(old_conf.value[0][1]['dir'], mnt)
        self.assertEqual(old_conf.value[0][1]['type'], fstype)
        self.assertEqual(old_conf.value[0][1]['opts'], opts)
        self.assertEqual(old_conf.value[0][1]['passno'], 0)
        self.assertEqual(old_conf.value[0][1]['freq'], 0)

        # update the configuration
        new_opts = self.str_to_ay('defaults,noauto')
        new_conf = copy.deepcopy(old_conf.value)
        new_conf[0][1]['opts'] = new_opts

        disk.UpdateConfigurationItem((old_conf.value[0][0], old_conf.value[0][1]), (new_conf[0][0], new_conf[0][1]),
                                     self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # get the configuration after the update
        upd_conf = self.get_property(disk, '.Block', 'Configuration')
        upd_conf.assertTrue()
        upd_conf.assertEqual(new_opts, getter=lambda c: c[0][1]['opts'])

        # this might fail in case of stray udev rules or records in /etc/fstab
        hint_auto = self.get_property_raw(disk, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(False))
        hint_ignore = self.get_property_raw(disk, '.Block', 'HintIgnore')
        self.assertEqual(hint_ignore, dbus.Boolean(False))
        hint_partitionable = self.get_property_raw(disk, '.Block', 'HintPartitionable')
        self.assertEqual(hint_partitionable, dbus.Boolean(True))
        hint_system = self.get_property_raw(disk, '.Block', 'HintSystem')
        self.assertEqual(hint_system, dbus.Boolean(True))

        # remove the configuration
        disk.RemoveConfigurationItem((upd_conf.value[0][0], upd_conf.value[0][1]),
                                     self.no_options, dbus_interface=self.iface_prefix + '.Block')

        upd_conf = self.get_property(disk, '.Block', 'Configuration')
        upd_conf.assertFalse()


    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSAFE)
    def test_configuration_crypttab(self):

        # this test will change /etc/crypttab, we might want to revert the changes when it finishes
        crypttab = self.read_file('/etc/crypttab')
        self.addCleanup(self.write_file, '/etc/crypttab', crypttab)

        # format the disk
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        disk.Format('xfs', {'encrypt.passphrase': self.LUKS_PASSPHRASE}, dbus_interface=self.iface_prefix + '.Block')

        # cleanup -- close the luks and remove format
        self.addCleanup(self.wipe_fs, self.vdevs[0])
        self.addCleanup(self._close_luks, disk)

        # configuration items as arrays of dbus.Byte
        opts = self.str_to_ay('verify')
        passwd = self.str_to_ay(self.LUKS_PASSPHRASE)

        # set the new configuration
        conf = dbus.Dictionary({'passphrase-contents': passwd,
                                'options': opts}, signature=dbus.Signature('sv'))
        disk.AddConfigurationItem(('crypttab', conf), self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # get the configuration
        old_conf = self.get_property(disk, '.Block', 'Configuration')
        old_conf.assertTrue()
        self.assertEqual(old_conf.value[0][1]['options'], opts)

        # get the secret configuration (passphrase)
        sec_conf = disk.GetSecretConfiguration(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(sec_conf)
        self.assertEqual(sec_conf[0][1]['passphrase-contents'], passwd)

        # update the configuration
        new_opts = self.str_to_ay('verify,discard')
        new_conf = copy.deepcopy(sec_conf)
        new_conf[0][1]['options'] = new_opts

        disk.UpdateConfigurationItem((sec_conf[0][0], sec_conf[0][1]), (new_conf[0][0], new_conf[0][1]),
                                     self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # get the configuration after the update
        upd_conf = self.get_property(disk, '.Block', 'Configuration')
        upd_conf.assertTrue()
        upd_conf.assertEqual(new_opts, getter=lambda c: c[0][1]['options'])

        # remove the configuration
        disk.RemoveConfigurationItem((upd_conf.value[0][0], upd_conf.value[0][1]),
                                     self.no_options, dbus_interface=self.iface_prefix + '.Block')

        upd_conf = self.get_property(disk, '.Block', 'Configuration')
        upd_conf.assertFalse()

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSAFE)
    def test_configuration_crypttab_multiple_spaces(self):
        # this test will change /etc/crypttab, we might want to revert the changes when it finishes
        crypttab = self.read_file('/etc/crypttab')
        self.addCleanup(self.write_file, '/etc/crypttab', crypttab)

        # format the disk
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        disk.Format('xfs', {'encrypt.passphrase': self.LUKS_PASSPHRASE}, dbus_interface=self.iface_prefix + '.Block')

        # cleanup -- close the luks and remove format
        self.addCleanup(self.wipe_fs, self.vdevs[0])
        self.addCleanup(self._close_luks, disk)

        # write configuration to crypttab
        uuid = self.get_property(disk, '.Block', 'IdUUID')
        self.write_file('/etc/crypttab', '%s  UUID=%s\t none\n' % (self.vdevs[0], uuid.value))

        conf = self.get_property(disk, '.Block', 'Configuration')
        conf.assertTrue()

        self.assertEqual(conf.value[0][1]['name'], self.str_to_ay(self.vdevs[0]))
        self.assertEqual(conf.value[0][1]['device'], self.str_to_ay('UUID=%s' % uuid.value))

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSAFE)
    def test_configuration_crypttab_no_keyfile(self):
        # this test will change /etc/crypttab, we might want to revert the changes when it finishes
        crypttab = self.read_file('/etc/crypttab')
        self.addCleanup(self.write_file, '/etc/crypttab', crypttab)

        # format the disk
        disk1 = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        disk1.Format('xfs', {'encrypt.passphrase': self.LUKS_PASSPHRASE}, dbus_interface=self.iface_prefix + '.Block')

        # cleanup (run in reverse order) -- close the luks and remove format
        self.addCleanup(self.wipe_fs, self.vdevs[0])
        self.addCleanup(self._close_luks, disk1)

        # format the disk
        disk2 = self.get_object('/block_devices/' + os.path.basename(self.vdevs[1]))
        disk2.Format('xfs', {'encrypt.passphrase': self.LUKS_PASSPHRASE}, dbus_interface=self.iface_prefix + '.Block')

        # cleanup (run in reverse order) -- close the luks and remove format
        self.addCleanup(self.wipe_fs, self.vdevs[1])
        self.addCleanup(self._close_luks, disk2)

        # write configuration to crypttab
        # both "none" and "-" should be accepted as an empty/non-existing key file
        uuid1 = self.get_property(disk1, '.Block', 'IdUUID')
        uuid2 = self.get_property(disk2, '.Block', 'IdUUID')
        self.write_file('/etc/crypttab', '%s UUID=%s\tnone\n%s UUID=%s\t-\n' % (self.vdevs[0], uuid1.value,
                                                                                self.vdevs[1], uuid2.value))

        # get the secret configuration (passphrase)
        sec_conf = disk1.GetSecretConfiguration(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(sec_conf)
        self.assertEqual(sec_conf[0][1]['passphrase-path'], self.str_to_ay(''))

        # get the secret configuration (passphrase)
        sec_conf = disk2.GetSecretConfiguration(self.no_options, dbus_interface=self.iface_prefix + '.Block')
        self.assertIsNotNone(sec_conf)
        self.assertEqual(sec_conf[0][1]['passphrase-path'], self.str_to_ay(''))

    def test_rescan(self):

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        disk.Rescan(self.no_options, dbus_interface=self.iface_prefix + '.Block')

    def test_encrypt(self):
        disk = self.vdevs[0]
        device = self.get_device(disk)
        self.assertIsNotNone(device)

        d = dbus.Dictionary(signature='sv')
        d['passphrase'] = "shouldnotseeme"
        d['key-size'] = dbus.UInt32(256)
        d['cipher'] = "aes"
        d['cipher-mode'] = "cbc-essiv:sha256"
        d['resilience'] = "datashift" # required, otherwise won't work
        d['hash'] = "sha256"
        d['max-hotzone-size'] = dbus.UInt64(0)
        d['sector-size'] = dbus.UInt32(512)
        d['new-volume_key'] = True

        device.Encrypt(self.LUKS_PASSPHRASE, d, dbus_interface=self.iface_prefix + '.Block')

        # verify that device now has the .Encrypted interface
        device = self.get_device(disk)
        self.assertHasIface(device, "org.freedesktop.UDisks2.Encrypted")

        # verify that the newly encrypted device can be unlocked
        luks_obj = device.Unlock("shouldnotseeme", self.no_options,
                                  dbus_interface=self.iface_prefix + '.Encrypted')
        self.assertIsNotNone(luks_obj)
        ret, _ = self.run_command("ls /dev/mapper/luks*")
        self.assertEqual(ret, 0)


class UdisksBlockRemovableTest(udiskstestcase.UdisksTestCase):
    '''Extra block device tests over a scsi_debug removable device'''

    # taken from test_40_drive.py and modified
    def setUp(self):
        res, _ = self.run_command('modprobe scsi_debug removable=1')
        self.assertEqual(res, 0)
        self.udev_settle()
        dirs = []
        # wait until directory appears
        while len(dirs) < 1:
            dirs = glob.glob('/sys/bus/pseudo/drivers/scsi_debug/adapter*/host*/target*/*:*/block')
            time.sleep(0.1)

        self.cd_dev = os.listdir(dirs[0])
        self.assertEqual(len(self.cd_dev), 1)
        obj = self.get_object('/block_devices/' + self.cd_dev[0])
        self.cd_dev = '/dev/' + self.cd_dev[0]
        self.assertTrue(os.path.exists(self.cd_dev))
        self.assertHasIface(obj, self.iface_prefix + '.Block')

        self.cd_device = self.get_device(self.cd_dev)

    def tearDown(self):
        device = self.cd_dev.split('/')[-1]
        if os.path.exists('/sys/block/' + device):
            self.write_file('/sys/block/%s/device/delete' % device, '1')
            while os.path.exists(device):
                time.sleep(0.1)
            self.udev_settle()
            self.run_command('modprobe -r scsi_debug')

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSAFE)
    def test_configuration_fstab_removable(self):

        # this test will change /etc/fstab, we might want to revert the changes when it finishes
        fstab = self.read_file('/etc/fstab')
        self.addCleanup(self.write_file, '/etc/fstab', fstab)

        # this might fail in case of stray udev rules or records in /etc/fstab
        hint_auto = self.get_property_raw(self.cd_device, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(True))
        hint_ignore = self.get_property_raw(self.cd_device, '.Block', 'HintIgnore')
        self.assertEqual(hint_ignore, dbus.Boolean(False))
        hint_partitionable = self.get_property_raw(self.cd_device, '.Block', 'HintPartitionable')
        self.assertEqual(hint_partitionable, dbus.Boolean(True))
        hint_system = self.get_property_raw(self.cd_device, '.Block', 'HintSystem')
        self.assertEqual(hint_system, dbus.Boolean(False))

        # configuration items as arrays of dbus.Byte
        mnt = self.str_to_ay('/mnt/test')
        fstype = self.str_to_ay('xfs')
        opts = self.str_to_ay('defaults')

        # set the new configuration
        conf = dbus.Dictionary({'dir': mnt, 'type': fstype, 'opts': opts, 'freq': 0, 'passno': 0},
                               signature=dbus.Signature('sv'))
        self.cd_device.AddConfigurationItem(('fstab', conf), self.no_options,
                                            dbus_interface=self.iface_prefix + '.Block')

        hint_auto = self.get_property_raw(self.cd_device, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(True))

        # get the configuration
        old_conf = self.get_property(self.cd_device, '.Block', 'Configuration')
        old_conf.assertTrue()

        # update the configuration
        new_opts = self.str_to_ay('defaults,noauto')
        new_conf = copy.deepcopy(old_conf.value)
        new_conf[0][1]['opts'] = new_opts

        self.cd_device.UpdateConfigurationItem((old_conf.value[0][0], old_conf.value[0][1]), (new_conf[0][0], new_conf[0][1]),
                                               self.no_options, dbus_interface=self.iface_prefix + '.Block')

        # get the configuration after the update
        upd_conf = self.get_property(self.cd_device, '.Block', 'Configuration')
        upd_conf.assertTrue()
        upd_conf.assertEqual(new_opts, getter=lambda c: c[0][1]['opts'])

        # 'noauto' option specified, should be reflected in the HintAuto property
        hint_auto = self.get_property_raw(self.cd_device, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(False))

        # remove the configuration
        self.cd_device.RemoveConfigurationItem((upd_conf.value[0][0], upd_conf.value[0][1]),
                                               self.no_options, dbus_interface=self.iface_prefix + '.Block')

        upd_conf = self.get_property(self.cd_device, '.Block', 'Configuration')
        upd_conf.assertFalse()

        hint_auto = self.get_property_raw(self.cd_device, '.Block', 'HintAuto')
        self.assertEqual(hint_auto, dbus.Boolean(True))
