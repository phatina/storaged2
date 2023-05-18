import dbus
import os
import six
import time

import udiskstestcase

BLOCK_SIZE = 512


class UdisksPartitionTableTest(udiskstestcase.UdisksTestCase):
    '''This is a basic block device test suite'''

    def _remove_format(self, device):
        d = dbus.Dictionary(signature='sv')
        d['erase'] = True
        try:
            device.Format('empty', d, dbus_interface=self.iface_prefix + '.Block')
        except dbus.exceptions.DBusException:
            pass

    def _create_format(self, device, ftype):
        device.Format(ftype, self.no_options, dbus_interface=self.iface_prefix + '.Block')

    def _remove_partition(self, part):
        try:
            part.Delete(self.no_options, dbus_interface=self.iface_prefix + '.Partition')
        except dbus.exceptions.DBusException:
            pass

    def test_create_mbr_partition(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create msdos partition table
        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        pttype = self.get_property(disk, '.PartitionTable', 'Type')
        pttype.assertEqual('dos')

        part_type = '0x8e'  # 'Linux LVM' type, see https://en.wikipedia.org/wiki/Partition_type#PID_8Eh

        # first try to create partition with name -> should fail because mbr
        # doesn't support partition names
        msg = 'MBR partition table does not support names'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2), part_type, 'name',
                                        self.no_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        # create partition
        path1 = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2), part_type, '',
                                     self.no_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path1)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)

        # check dbus properties
        size = self.get_property(part, '.Partition', 'Size')
        size.assertEqual(100 * 1024**2)

        offset = self.get_property(part, '.Partition', 'Offset')
        offset.assertEqual(1024**2)

        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(part_type)

        # check system values
        part_name = path1.split('/')[-1]
        disk_name = os.path.basename(self.vdevs[0])
        part_syspath = '/sys/block/%s/%s' % (disk_name, part_name)
        self.assertTrue(os.path.isdir(part_syspath))

        sys_size = int(self.read_file('%s/size' % part_syspath))
        self.assertEqual(sys_size * BLOCK_SIZE, 100 * 1024**2)

        sys_start = int(self.read_file('%s/start' % part_syspath))
        self.assertEqual(sys_start * BLOCK_SIZE, 1024**2)

        # format the partition so blkid is able to display info about it
        # (yes, it is stupid, but this is how blkid works on CentOS/RHEL 7)
        _ret, _out = self.run_command('mkfs.ext2 /dev/%s' % part_name)

        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, part_type)

        # check uuid and part number
        dbus_uuid = self.get_property(part, '.Partition', 'UUID')
        _ret, sys_uuid = self.run_command('lsblk -d -no PARTUUID /dev/%s' % part_name)
        dbus_uuid.assertEqual(sys_uuid)

        dbus_num = self.get_property(part, '.Partition', 'Number')
        sys_num = int(self.read_file('%s/partition' % part_syspath))
        dbus_num.assertEqual(sys_num)

        # create another partition
        path2 = disk.CreatePartition(dbus.UInt64(1024**2 + (1024**2 + 100 * 1024**2)), dbus.UInt64(100 * 1024**2),
                                     part_type, '', self.no_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path2)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)

        # create yet another partition
        path3 = disk.CreatePartition(dbus.UInt64(1024**2 + 2 * (1024**2 + 100 * 1024**2)), dbus.UInt64(100 * 1024**2),
                                     part_type, '', self.no_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path3)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)

        # there should now be 3 partitions on the disk
        dbus_parts = self.get_property(disk, '.PartitionTable', 'Partitions')
        dbus_parts.assertLen(3)

        self.assertIn(path1, dbus_parts.value)
        self.assertIn(path2, dbus_parts.value)
        self.assertIn(path3, dbus_parts.value)

    def create_extended_partition(self, ext_options, log_options, part_type=''):

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create msdos partition table
        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # create extended partition
        ext_path = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(150 * 1024**2), part_type, '',
                                        ext_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        ext_part = self.bus.get_object(self.iface_prefix, ext_path)
        self.assertIsNotNone(ext_part)

        self.addCleanup(self._remove_partition, ext_part)

        # check dbus type (0x05, 0x0f, 0x85 are all exteded types, see https://en.wikipedia.org/wiki/Partition_type#PID_05h)
        dbus_pttype = self.get_property(ext_part, '.Partition', 'Type')
        dbus_pttype.assertIn(['0x05', '0x0f', '0x85'])

        # check if its a 'container'
        dbus_cont = self.get_property(ext_part, '.Partition', 'IsContainer')
        dbus_cont.assertTrue()

        # check system type
        part_name = str(ext_part.object_path).split('/')[-1]
        _ret, sys_pttype = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertIn(sys_pttype, ['0x5', '0xf', '0x85'])  # lsblk prints 0xf instead of 0x0f

        # create logical partition
        log_path = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(50 * 1024**2), '', '',
                                        log_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        log_part = self.bus.get_object(self.iface_prefix, log_path)
        self.assertIsNotNone(log_part)

        self.addCleanup(self._remove_partition, log_part)

        # check if its a 'contained'
        dbus_cont = self.get_property(log_part, '.Partition', 'IsContained')
        dbus_cont.assertTrue()

        dbus_offset = self.get_property(log_part, '.Partition', 'Offset')
        dbus_size = self.get_property(log_part, '.Partition', 'Size')

        # create one more logical partition
        log_path2 = disk.CreatePartition(dbus.UInt64(dbus_offset.value + dbus_size.value), dbus.UInt64(50 * 1024**2), '', '',
                                         log_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        log_part2 = self.bus.get_object(self.iface_prefix, log_path2)
        self.assertIsNotNone(log_part2)

        self.addCleanup(self._remove_partition, log_part2)

    def test_create_extended_partition(self):
        self.create_extended_partition(self.no_options, self.no_options, '0x05')

    def test_create_explicit_extended_partition(self):
        ext_options = dbus.Dictionary({'partition-type': 'extended'}, signature='sv')
        log_options = dbus.Dictionary({'partition-type': 'logical'}, signature='sv')
        self.create_extended_partition(ext_options, log_options)

    def test_fill_with_primary_partitions(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create msdos partition table
        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        options = dbus.Dictionary({'partition-type': 'primary'}, signature='sv')
        offset = 1024**2
        size = 10 * 1024**2
        for i in range(4):
            # create primary partition
            path = disk.CreatePartition(dbus.UInt64(offset + i * (offset + size)), dbus.UInt64(size), '', '',
                                            options, dbus_interface=self.iface_prefix + '.PartitionTable')

            part = self.bus.get_object(self.iface_prefix, path)
            self.assertIsNotNone(part)
            self.addCleanup(self._remove_partition, part)

            dbus_cont = self.get_property(part, '.Partition', 'IsContainer')
            dbus_cont.assertFalse()

            dbus_cont = self.get_property(part, '.Partition', 'IsContained')
            dbus_cont.assertFalse()

    def test_create_gpt_partition(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create gpt partition table
        self._create_format(disk, 'gpt')
        pttype = self.get_property(disk, '.PartitionTable', 'Type')
        pttype.assertEqual('gpt')

        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        gpt_name = 'home'
        gpt_type = '933ac7e1-2eb4-4f13-b844-0e14e2aef915'

        # create partition
        path = disk.CreatePartition(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2),
                                    gpt_type, gpt_name,
                                    self.no_options, dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)

        # check dbus properties
        size = self.get_property(part, '.Partition', 'Size')
        size.assertEqual(100 * 1024**2)

        offset = self.get_property(part, '.Partition', 'Offset')
        offset.assertEqual(1024**2)

        dbus_name = self.get_property(part, '.Partition', 'Name')
        dbus_name.assertEqual(gpt_name)

        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(gpt_type)

        # check system values
        part_name = path.split('/')[-1]
        disk_name = os.path.basename(self.vdevs[0])
        part_syspath = '/sys/block/%s/%s' % (disk_name, part_name)
        self.assertTrue(os.path.isdir(part_syspath))

        sys_size = int(self.read_file('%s/size' % part_syspath))
        self.assertEqual(sys_size * BLOCK_SIZE, 100 * 1024**2)

        sys_start = int(self.read_file('%s/start' % part_syspath))
        self.assertEqual(sys_start * BLOCK_SIZE, 1024**2)

        _ret, sys_name = self.run_command('lsblk -d -no PARTLABEL /dev/%s' % part_name)
        self.assertEqual(sys_name, gpt_name)

        # format the partition so blkid is able to display info about it
        # (yes, it is stupid, but this is how blkid works on CentOS/RHEL 7)
        _ret, _out = self.run_command('mkfs.ext2 /dev/%s' % part_name)

        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, gpt_type)

    def test_create_with_format(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create msdos partition table
        self._create_format(disk, 'dos')

        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # create partition with ext4 format
        path = disk.CreatePartitionAndFormat(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2), '', '',
                                             self.no_options, 'ext4', self.no_options,
                                             dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)
        self.addCleanup(self._remove_format, part)

        # check dbus properties
        size = self.get_property(part, '.Partition', 'Size')
        size.assertEqual(100 * 1024**2)

        offset = self.get_property(part, '.Partition', 'Offset')
        offset.assertEqual(1024**2)

        usage = self.get_property(part, '.Block', 'IdUsage')
        usage.assertEqual('filesystem')

        fstype = self.get_property(part, '.Block', 'IdType')
        fstype.assertEqual('ext4')

        # check system values
        part_name = path.split('/')[-1]
        disk_name = os.path.basename(self.vdevs[0])
        part_syspath = '/sys/block/%s/%s' % (disk_name, part_name)
        self.assertTrue(os.path.isdir(part_syspath))

        sys_size = int(self.read_file('%s/size' % part_syspath))
        self.assertEqual(sys_size * BLOCK_SIZE, 100 * 1024**2)

        sys_start = int(self.read_file('%s/start' % part_syspath))
        self.assertEqual(sys_start * BLOCK_SIZE, 1024**2)

        _ret, sys_fstype = self.run_command('lsblk -d -no FSTYPE /dev/%s' % part_name)
        self.assertEqual(sys_fstype, 'ext4')

    def _have_udftools(self):
        ret, _out = self.run_command('type mkudffs')
        return ret == 0

    def test_create_with_format_auto_type_mbr(self):
        if not self._have_udftools():
            self.skipTest('Udftools needed to check automatic partition type update.')

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create msdos partition table
        self._create_format(disk, 'dos')

        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # create partition with udf format and automatically set partition type
        # it should be 0x07
        d = dbus.Dictionary(signature='sv')
        d['update-partition-type'] = True
        path = disk.CreatePartitionAndFormat(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2), '', '',
                                             self.no_options, 'udf', d,
                                             dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)
        self.addCleanup(self._remove_format, part)

        # check dbus properties
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual('0x07')

        # check system values
        part_name = path.split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, '0x7')

    def test_create_with_format_auto_type_gpt(self):
        if not self._have_udftools():
            self.skipTest('Udftools needed to check automatic partition type update.')

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        # create gpt partition table
        self._create_format(disk, 'gpt')

        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        # create partition with udf format and automatically set partition type
        # it should be ebd0a0a2-b9e5-4433-87c0-68b6b72699c7
        d = dbus.Dictionary(signature='sv')
        d['update-partition-type'] = True
        path = disk.CreatePartitionAndFormat(dbus.UInt64(1024**2), dbus.UInt64(100 * 1024**2), '', '',
                                             self.no_options, 'udf', d,
                                             dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        self.addCleanup(self._remove_partition, part)
        self.addCleanup(self._remove_format, part)

        # check dbus properties
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')

        # check system values
        part_name = path.split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')


class UdisksPartitionTest(udiskstestcase.UdisksTestCase):
    '''This is a basic partition test suite'''

    def _remove_format(self, device):
        d = dbus.Dictionary(signature='sv')
        d['erase'] = True
        try:
            device.Format('empty', d, dbus_interface=self.iface_prefix + '.Block')
        except dbus.exceptions.DBusException:
            pass

    def _create_format(self, device, ftype):
        device.Format(ftype, self.no_options, dbus_interface=self.iface_prefix + '.Block')

    def _remove_partition(self, part):
        try:
            part.Delete(self.no_options, dbus_interface=self.iface_prefix + '.Partition')
        except dbus.exceptions.DBusException:
            pass

    def _create_partition(self, disk, start=1024**2, size=100 * 1024**2, fmt='ext4', type=''):
        if fmt:
            path = disk.CreatePartitionAndFormat(dbus.UInt64(start), dbus.UInt64(size), type, '',
                                                 self.no_options, fmt, self.no_options,
                                                 dbus_interface=self.iface_prefix + '.PartitionTable')
        else:
            path = disk.CreatePartition(dbus.UInt64(start), dbus.UInt64(size), type, '',
                                        self.no_options,
                                        dbus_interface=self.iface_prefix + '.PartitionTable')

        part = self.bus.get_object(self.iface_prefix, path)
        self.assertIsNotNone(part)

        return part

    def test_delete(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk)
        path = str(part.object_path)

        part.Delete(self.no_options, dbus_interface=self.iface_prefix + '.Partition')

        part_name = path.split('/')[-1]
        disk_name = os.path.basename(self.vdevs[0])
        part_syspath = '/sys/block/%s/%s' % (disk_name, part_name)

        # make sure the partition is not on dbus
        udisks = self.get_object('')
        objects = udisks.GetManagedObjects(dbus_interface='org.freedesktop.DBus.ObjectManager')
        self.assertNotIn(path, objects.keys())

        # make sure partition is not in the system
        self.assertFalse(os.path.isdir(part_syspath))

        # partitions property of the disk should be updated too
        dbus_parts = self.get_property(disk, '.PartitionTable', 'Partitions')
        dbus_parts.assertFalse()

    def test_dos_flags(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk)
        self.addCleanup(self._remove_partition, part)

        self._create_format(part, 'ext4')
        self.addCleanup(self._remove_format, part)

        # set boot flag (10000000(2), 128(10), 0x80(16))
        part.SetFlags(dbus.UInt64(128), self.no_options,
                      dbus_interface=self.iface_prefix + '.Partition')

        # test flags value on types
        dbus_flags = self.get_property(part, '.Partition', 'Flags')
        dbus_flags.assertEqual(128)

        # test flags value from system
        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_flags = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_FLAGS' % part_name)
        self.assertEqual(sys_flags, '0x80')

    def test_gpt_flags(self):
        esp_guid = 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'

        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'gpt')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk, fmt=False, type=esp_guid)
        self.addCleanup(self._remove_partition, part)

        # test partition type
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(esp_guid)

        # test partition type from system
        part_name = str(part.object_path).split('/')[-1]

        # format the partition so blkid is able to display info about it
        # (yes, it is stupid, but this is how blkid works on CentOS/RHEL 7)
        _ret, _out = self.run_command('mkfs.ext2 /dev/%s' % part_name)

        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, esp_guid)

        # set legacy BIOS bootable flag (100(2), 4(10), 0x4(16))
        part.SetFlags(dbus.UInt64(4), self.no_options,
                      dbus_interface=self.iface_prefix + '.Partition')

        # test flags value on types
        dbus_flags = self.get_property(part, '.Partition', 'Flags')
        dbus_flags.assertEqual(4)

        _ret, sys_flags = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_FLAGS' % part_name)
        self.assertEqual(sys_flags, '0x4')

        # test partition type
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(esp_guid)

        # test partition type from system
        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, esp_guid)

    def test_gpt_type(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'gpt')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk)
        self.addCleanup(self._remove_partition, part)

        self._create_format(part, 'ext4')
        self.addCleanup(self._remove_format, part)

        # first try some invalid guid
        msg = 'org.freedesktop.UDisks2.Error.Failed: .* is not a valid UUID'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            part.SetType('aaaa', self.no_options,
                         dbus_interface=self.iface_prefix + '.Partition')

        # set part type/guid (home partition)
        home_guid = '933ac7e1-2eb4-4f13-b844-0e14e2aef915'
        part.SetType(home_guid, self.no_options,
                     dbus_interface=self.iface_prefix + '.Partition')

        # test flags value on types
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(home_guid)

        # test flags value from system
        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, home_guid)

    def test_dos_type(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'dos')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk)
        self.addCleanup(self._remove_partition, part)

        self._create_format(part, 'ext4')
        self.addCleanup(self._remove_format, part)

        # try to set part type to an extended partition type -- should fail
        msg = 'Refusing to change partition type to that of an extended partition'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            part.SetType('0x05', self.no_options,
                         dbus_interface=self.iface_prefix + '.Partition')

        # set part type/id
        part_type = '0x8e'   # 'Linux LVM' type, see https://en.wikipedia.org/wiki/Partition_type#PID_8Eh
        part.SetType(part_type, self.no_options,
                     dbus_interface=self.iface_prefix + '.Partition')

        # test flags value on types
        dbus_type = self.get_property(part, '.Partition', 'Type')
        dbus_type.assertEqual(part_type)

        # test flags value from system
        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_type = self.run_command('blkid /dev/%s -p -o value -s PART_ENTRY_TYPE' % part_name)
        self.assertEqual(sys_type, part_type)

    @udiskstestcase.tag_test(udiskstestcase.TestTags.UNSTABLE)
    def test_resize(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'gpt')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        disk_size = self.get_property(disk, '.Block', 'Size')

        # span almost whole drive (minus ~8 MiB)
        part = self._create_partition(disk, size=disk_size.value - 8 * 1024**2, fmt=None)
        self.addCleanup(self._remove_partition, part)

        part_size = self.get_property(part, '.Partition', 'Size')
        part_offset = self.get_property(part, '.Partition', 'Offset')
        initial_offset = part_offset.value
        initial_size = part_size.value

        # adding 30 MiB should be too big
        with self.assertRaises(dbus.exceptions.DBusException):
            part.Resize(disk_size.value + 30 * 1024**2, self.no_options,
                        dbus_interface=self.iface_prefix + '.Partition')

        # no harm should happen for failures
        part_size.assertEqual(initial_size)
        part_offset.assertEqual(initial_offset)

        # resize to maximum
        part.Resize(0, self.no_options,
                    dbus_interface=self.iface_prefix + '.Partition')

        part_offset.assertEqual(initial_offset)
        max_size = part_size.value
        part_size.assertGreater(initial_size)
        part_size.assertLess(disk_size.value)

        new_size = 13 * 1000**2  # MB (not MiB) as non-multiple of the block size
        part.Resize(new_size, self.no_options,
                    dbus_interface=self.iface_prefix + '.Partition')

        part_offset.assertEqual(initial_offset)
        # resize should guarantee at least the requested size
        part_size.assertGreater(new_size - 1)
        part_size.assertLess(new_size + 1 * 1024**2)  # assuming 1 MiB alignment

        # resize to maximum explicitly
        part.Resize(max_size, self.no_options,
                    dbus_interface=self.iface_prefix + '.Partition')

        part_offset.assertEqual(initial_offset)
        part_size.assertGreater(max_size - 1)
        part_size.assertLess(disk_size.value)

    def test_name(self):
        disk = self.get_object('/block_devices/' + os.path.basename(self.vdevs[0]))
        self.assertIsNotNone(disk)

        self._create_format(disk, 'gpt')
        self.addCleanup(self._remove_format, disk)
        self.addCleanup(self.wipe_fs, self.vdevs[0])

        part = self._create_partition(disk)
        self.addCleanup(self._remove_partition, part)

        self._create_format(part, 'ext4')
        self.addCleanup(self._remove_format, part)

        # first try some invalid name (longer than 36 characters)
        msg = 'Max partition name length is 36 characters'
        with six.assertRaisesRegex(self, dbus.exceptions.DBusException, msg):
            part.SetName('a' * 37, self.no_options,
                         dbus_interface=self.iface_prefix + '.Partition')

        # set part name
        part.SetName('test', self.no_options,
                     dbus_interface=self.iface_prefix + '.Partition')

        # test flags value on types
        dbus_name = self.get_property(part, '.Partition', 'Name')
        dbus_name.assertEqual('test')

        # test flags value from system
        part_name = str(part.object_path).split('/')[-1]
        _ret, sys_name = self.run_command('lsblk -d -no PARTLABEL /dev/%s' % part_name)
        self.assertEqual(sys_name, 'test')
