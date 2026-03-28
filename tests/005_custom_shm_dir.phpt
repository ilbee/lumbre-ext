--TEST--
lumbre: custom shm_dir places ring buffer file in specified directory
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
@mkdir('/dev/shm/lumbre_test_005', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_005
--FILE--
<?php
$shm_dir = '/dev/shm/lumbre_test_005';

/* Trigger ring buffer init via a file I/O call */
$tmp = tempnam(sys_get_temp_dir(), 'lumbre_test_');
file_put_contents($tmp, 'test data');
$data = file_get_contents($tmp);
unlink($tmp);

/* Verify ring buffer file is in our custom dir, not /dev/shm */
$files = glob($shm_dir . '/lumbre_*');
$found = is_array($files) && count($files) > 0;
var_dump($found);

/* Verify it is NOT in /dev/shm (with our PID) */
$pid = getmypid();
$default_files = glob('/dev/shm/lumbre_' . $pid . '_*');
$in_default = is_array($default_files) && count($default_files) > 0;
var_dump($in_default);

echo "done\n";
?>
--CLEAN--
<?php
$shm_dir = '/dev/shm/lumbre_test_005';
$files = glob($shm_dir . '/lumbre_*');
if (is_array($files)) {
    foreach ($files as $f) {
        @unlink($f);
    }
}
@rmdir($shm_dir);
?>
--EXPECT--
bool(true)
bool(false)
done
