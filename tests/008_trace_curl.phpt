--TEST--
lumbre: curl_exec creates HTTP_OUT span and ring buffer
--SKIPIF--
<?php
if (!extension_loaded('lumbre')) die('skip lumbre not loaded');
if (!extension_loaded('curl')) die('skip curl not loaded');
if (!is_dir('/dev/shm')) die('skip /dev/shm not available');
@mkdir('/dev/shm/lumbre_test_008', 0777, true);
?>
--INI--
lumbre.enabled=1
lumbre.mode=io
lumbre.shm_dir=/dev/shm/lumbre_test_008
lumbre.buffer_size=65536
--FILE--
<?php
$shmDir = '/dev/shm/lumbre_test_008';

// Use curl with file:// to avoid needing an HTTP server
$tmpFile = tempnam(sys_get_temp_dir(), 'curl_');
file_put_contents($tmpFile, 'curl_test_payload');

$ch = curl_init("file://{$tmpFile}");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 3);
$result = curl_exec($ch);
$errno = curl_errno($ch);
curl_close($ch);

echo "curl_result=" . ($result === 'curl_test_payload' ? "ok" : "fail") . "\n";
echo "curl_errno=" . $errno . "\n";

// Also test with an unreachable HTTP URL — lumbre hook must not crash
$ch2 = curl_init("http://127.0.0.1:19999/nonexistent");
curl_setopt($ch2, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch2, CURLOPT_TIMEOUT, 1);
curl_setopt($ch2, CURLOPT_CONNECTTIMEOUT, 1);
curl_exec($ch2);
curl_close($ch2);

echo "curl_fail_handled=ok\n";

// Check ring buffer file was created
$files = glob($shmDir . '/lumbre_*');
echo "shm_files=" . count($files) . "\n";
if (count($files) > 0) {
    echo "buffer_has_data=" . (filesize($files[0]) > 64 ? "yes" : "no") . "\n";
} else {
    echo "buffer_has_data=no\n";
}

@unlink($tmpFile);
?>
--CLEAN--
<?php
$d = '/dev/shm/lumbre_test_008';
foreach (glob($d . '/*') as $f) @unlink($f);
@rmdir($d);
?>
--EXPECTF--
curl_result=ok
curl_errno=0
curl_fail_handled=ok
shm_files=1
buffer_has_data=yes
