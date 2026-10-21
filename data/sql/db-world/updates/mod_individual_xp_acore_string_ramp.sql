SET @ENTRY:=100000;
DELETE FROM `acore_string` WHERE `entry` = @ENTRY+10;
INSERT INTO `acore_string` (`entry`, `content_default`, `locale_esES`, `locale_esMX`) VALUES
(@ENTRY+10, '|cffffffff[XP] Until level {} it grows with your level: {} right now.|r', '|cffffffff[XP] Hasta el nivel {} crece con tu nivel: {} ahora mismo.|r', '|cffffffff[XP] Hasta el nivel {} crece con tu nivel: {} ahora mismo.|r');

-- .xp set takes rates down to 0.1, not 1.
UPDATE `acore_string` SET
    `content_default` = '[XP] The minimum rate limit is 0.1.',
    `locale_esES` = '[XP] El límite mínimo de XP es 0.1.',
    `locale_esMX` = '[XP] El límite mínimo de XP es 0.1.'
WHERE `entry` = @ENTRY+5;
