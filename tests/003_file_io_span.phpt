--TEST--
lumbre: file_get_contents creates shm ring buffer file
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
@mkdir('/dev/shm/lumbre_test_003', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_003
--FILE--
<?php
$shm_dir = '/dev/shm/lumbre_test_003';

/* Trigger a file I/O operation so the ring buffer gets initialized */
$tmp = tempnam(sys_get_temp_dir(), 'lumbre_test_');
file_put_contents($tmp, 'hello');
$data = file_get_contents($tmp);
unlink($tmp);

/* Check that a ring buffer file was created in shm_dir */
$files = glob($shm_dir . '/lumbre_*');
$found = is_array($files) && count($files) > 0;
var_dump($found);

if ($found) {
    /* Verify filename pattern: lumbre_{pid}_{worker_id} */
    $basename = basename($files[0]);
    $matches = preg_match('/^lumbre_\d+_\d+$/', $basename);
    var_dump($matches === 1);
}

echo "done\n";
?>
--CLEAN--
<?php
$shm_dir = '/dev/shm/lumbre_test_003';
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
bool(true)
done
