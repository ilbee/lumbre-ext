--TEST--
lumbre: INI settings have correct defaults
--INI--
lumbre.enabled=0
--FILE--
<?php
$defaults = array(
    'lumbre.enabled'          => '1',
    'lumbre.mode'             => 'io',
    'lumbre.buffer_size'      => '4194304',
    'lumbre.shm_dir'          => '/dev/shm',
    'lumbre.trigger_header'   => 'X-Trace-Debug',
    'lumbre.max_query_len'    => '2048',
    'lumbre.min_duration_ns'  => '0',
    'lumbre.trace_namespaces' => '',
);

foreach ($defaults as $key => $expected) {
    $actual = ini_get($key);
    /* lumbre.enabled was overridden to 0 in --INI--, skip its default check */
    if ($key === 'lumbre.enabled') {
        echo "lumbre.enabled: skip (overridden)\n";
        continue;
    }
    if ($actual === $expected) {
        echo "$key: OK\n";
    } else {
        echo "$key: FAIL (expected '$expected', got '$actual')\n";
    }
}

echo "done\n";
?>
--EXPECT--
lumbre.enabled: skip (overridden)
lumbre.mode: OK
lumbre.buffer_size: OK
lumbre.shm_dir: OK
lumbre.trigger_header: OK
lumbre.max_query_len: OK
lumbre.min_duration_ns: OK
lumbre.trace_namespaces: OK
done
