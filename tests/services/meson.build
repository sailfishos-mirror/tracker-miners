# This directory contains service files used by dbus-daemon to automatically
# activate the daemons as needed. These files are used when running Tracker
# from the build tree.

tracker_miner_fs_test_dbus_service_file = configure_file(
    input: 'org.freedesktop.Tracker3.Miner.Files.service.in',
    output: 'org.freedesktop.Tracker3.Miner.Files.service',
    configuration: conf)

tracker_index_test_dbus_service_file = configure_file(
    input: 'org.freedesktop.Tracker3.Miner.Files.Control.service.in',
    output: 'org.freedesktop.Tracker3.Miner.Files.Control.service',
    configuration: conf)

tracker_writeback_test_dbus_service_file = configure_file(
    input: 'org.freedesktop.Tracker3.Writeback.service.in',
    output: 'org.freedesktop.Tracker3.Writeback.service',
    configuration: conf)

subdir('miners')
