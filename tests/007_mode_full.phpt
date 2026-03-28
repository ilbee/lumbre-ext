--TEST--
lumbre: mode "full" is accepted and reflected in phpinfo
--INI--
lumbre.enabled=0
lumbre.mode=full
--FILE--
<?php
$mode = ini_get('lumbre.mode');
var_dump($mode);

/* Verify phpinfo shows "full" mode */
ob_start();
phpinfo(INFO_MODULES);
$info = ob_get_clean();

$found = (strpos($info, 'full') !== false);
var_dump($found);

echo "done\n";
?>
--EXPECT--
string(4) "full"
bool(true)
done
