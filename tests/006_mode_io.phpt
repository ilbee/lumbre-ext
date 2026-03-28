--TEST--
lumbre: mode "io" is accepted and reflected in INI
--INI--
lumbre.enabled=0
lumbre.mode=io
--FILE--
<?php
$mode = ini_get('lumbre.mode');
var_dump($mode);

/* Verify phpinfo shows "io" mode */
ob_start();
phpinfo(INFO_MODULES);
$info = ob_get_clean();

$found = (strpos($info, 'io') !== false);
var_dump($found);

echo "done\n";
?>
--EXPECT--
string(2) "io"
bool(true)
done
