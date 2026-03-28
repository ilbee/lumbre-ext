--TEST--
lumbre: extension loads and phpinfo shows lumbre section
--INI--
lumbre.enabled=0
--FILE--
<?php
var_dump(extension_loaded('lumbre'));

ob_start();
phpinfo(INFO_MODULES);
$info = ob_get_clean();

$found = (strpos($info, 'lumbre support') !== false);
var_dump($found);

$found_version = (strpos($info, 'Version') !== false);
var_dump($found_version);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
done
