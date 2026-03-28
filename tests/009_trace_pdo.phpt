--TEST--
lumbre: PDO::query and PDO::execute create DB spans and ring buffer
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!extension_loaded('pdo_sqlite')) die('skip pdo_sqlite not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
@mkdir('/dev/shm/lumbre_test_009', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_009
lumbre.buffer_size=65536
--FILE--
<?php
$shmDir = '/dev/shm/lumbre_test_009';

$dbFile = tempnam(sys_get_temp_dir(), 'pdo_');

$pdo = new PDO("sqlite:{$dbFile}");
$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// PDO::query — should trigger a DB span
$pdo->query("CREATE TABLE test_spans (id INTEGER PRIMARY KEY, name TEXT)");
echo "create_table=ok\n";

// PDO::execute via prepared statement — should trigger another DB span
$stmt = $pdo->prepare("INSERT INTO test_spans (name) VALUES (?)");
$stmt->execute(array('span_test'));
echo "insert=ok\n";

// Verify data
$row = $pdo->query("SELECT name FROM test_spans LIMIT 1")->fetch(PDO::FETCH_ASSOC);
echo "select=" . $row['name'] . "\n";

// Check ring buffer
$files = glob($shmDir . '/lumbre_*');
echo "shm_files=" . count($files) . "\n";
if (count($files) > 0) {
    echo "buffer_has_data=" . (filesize($files[0]) > 64 ? "yes" : "no") . "\n";
} else {
    echo "buffer_has_data=no\n";
}

$pdo = null;
@unlink($dbFile);
?>
--CLEAN--
<?php
$d = '/dev/shm/lumbre_test_009';
foreach (glob($d . '/*') as $f) @unlink($f);
@rmdir($d);
?>
--EXPECTF--
create_table=ok
insert=ok
select=span_test
shm_files=1
buffer_has_data=yes
