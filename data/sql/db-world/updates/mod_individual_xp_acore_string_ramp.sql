SET @ENTRY:=100000;
DELETE FROM `acore_string` WHERE `entry` = @ENTRY+10;
INSERT INTO `acore_string` (`entry`, `content_default`) VALUES
(@ENTRY+10, '|cffffffff[XP] Until level {} it grows with your level: {} right now.|r');

-- .xp set takes rates down to 0.1, not 1.
UPDATE `acore_string` SET `content_default` = '[XP] The minimum rate limit is 0.1.' WHERE `entry` = @ENTRY+5;

DELETE FROM `acore_string_locale` WHERE `entry` IN (@ENTRY+5, @ENTRY+10) AND `locale` IN ('esES', 'esMX');
INSERT INTO `acore_string_locale` (`entry`, `locale`, `content`) VALUES
(@ENTRY+5, 'esES', '[XP] El límite mínimo de XP es 0.1.'),
(@ENTRY+5, 'esMX', '[XP] El límite mínimo de XP es 0.1.'),
(@ENTRY+10, 'esES', '|cffffffff[XP] Hasta el nivel {} crece con tu nivel: {} ahora mismo.|r'),
(@ENTRY+10, 'esMX', '|cffffffff[XP] Hasta el nivel {} crece con tu nivel: {} ahora mismo.|r');
