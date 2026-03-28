--TEST--
lumbre: buffer overflow drops spans without crash
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!extension_loaded('pdo_sqlite')) die('skip pdo_sqlite not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
@mkdir('/dev/shm/lumbre_test_011', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_011
lumbre.buffer_size=4096
--FILE--
<?php
$shmDir = '/dev/shm/lumbre_test_011';

// Tiny buffer (4096 bytes). Header = 64 bytes, data capacity ~4032.
// Each span is ~100-300 bytes, so ~15-40 fit. We do 200 to guarantee overflow.

$dbFile = tempnam(sys_get_temp_dir(), 'ovf_');
$pdo = new PDO("sqlite:{$dbFile}");
$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$pdo->query("CREATE TABLE overflow_test (id INTEGER PRIMARY KEY, val TEXT)");

$overflowCount = 200;
for ($i = 0; $i < $overflowCount; $i++) {
    $pdo->query("INSERT INTO overflow_test (val) VALUES ('span_{$i}')");
}

echo "writes_completed=ok\n";
echo "no_crash=yes\n";

// Verify PHP still works normally after overflow
$row = $pdo->query("SELECT COUNT(*) AS c FROM overflow_test")->fetch(PDO::FETCH_ASSOC);
echo "rows_inserted=" . $row['c'] . "\n";

// Ring buffer file should still exist
$files = glob($shmDir . '/lumbre_*');
echo "shm_files=" . count($files) . "\n";

$pdo = null;
@unlink($dbFile);
?>
--CLEAN--
<?php
$d = '/dev/shm/lumbre_test_011';
foreach (glob($d . '/*') as $f) @unlink($f);
@rmdir($d);
?>
--EXPECTF--
writes_completed=ok
no_crash=yes
rows_inserted=200
shm_files=1
