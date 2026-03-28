--TEST--
lumbre: Redis::set and Redis::get create REDIS spans and ring buffer
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!extension_loaded('redis')) die('skip redis not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
$host = getenv('REDIS_HOST') ?: '127.0.0.1';
$port = (int)(getenv('REDIS_PORT') ?: 6379);
$r = new Redis();
if (!@$r->connect($host, $port, 1.0)) die('skip cannot connect to Redis at ' . $host . ':' . $port);
$r->close();
@mkdir('/dev/shm/lumbre_test_010', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_010
lumbre.buffer_size=65536
--FILE--
<?php
$shmDir = '/dev/shm/lumbre_test_010';
$host = getenv('REDIS_HOST') ?: '127.0.0.1';
$port = (int)(getenv('REDIS_PORT') ?: 6379);

$redis = new Redis();
$redis->connect($host, $port, 1.0);

// Redis::set — should trigger a REDIS span
$setResult = $redis->set('lumbre_test_key', 'hello_lumbre');
echo "set=" . ($setResult ? "ok" : "fail") . "\n";

// Redis::get — should trigger another REDIS span
$value = $redis->get('lumbre_test_key');
echo "get=" . $value . "\n";

// Redis::mget — should trigger another REDIS span
$values = $redis->mget(array('lumbre_test_key', 'nonexistent'));
echo "mget_count=" . count($values) . "\n";

$redis->del('lumbre_test_key');
$redis->close();

// Check ring buffer
$files = glob($shmDir . '/lumbre_*');
echo "shm_files=" . count($files) . "\n";
if (count($files) > 0) {
    echo "buffer_has_data=" . (filesize($files[0]) > 64 ? "yes" : "no") . "\n";
} else {
    echo "buffer_has_data=no\n";
}
?>
--CLEAN--
<?php
$d = '/dev/shm/lumbre_test_010';
foreach (glob($d . '/*') as $f) @unlink($f);
@rmdir($d);
?>
--EXPECTF--
set=ok
get=hello_lumbre
mget_count=2
shm_files=1
buffer_has_data=yes
