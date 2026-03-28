--TEST--
lumbre: when disabled, no shm files are created
--INI--
lumbre.enabled=0
lumbre.shm_dir=/tmp/lumbre_test_004
--FILE--
<?php
$shm_dir = '/tmp/lumbre_test_004';
@mkdir($shm_dir, 0777, true);

/* Perform file I/O — should NOT trigger ring buffer creation */
$tmp = tempnam(sys_get_temp_dir(), 'lumbre_test_');
file_put_contents($tmp, 'hello');
$data = file_get_contents($tmp);
unlink($tmp);

/* No ring buffer file should exist */
$files = glob($shm_dir . '/lumbre_*');
$found = is_array($files) && count($files) > 0;
var_dump($found);

echo "done\n";
?>
--CLEAN--
<?php
$shm_dir = '/tmp/lumbre_test_004';
$files = glob($shm_dir . '/lumbre_*');
if (is_array($files)) {
    foreach ($files as $f) {
        @unlink($f);
    }
}
@rmdir($shm_dir);
?>
--EXPECT--
bool(false)
done
