-- mod-hearthside-pvp: world database (pvp_life_zone, pvp_life_chat, the .wpvp command row).
-- One idempotent file, applied by AzerothCore's updater and re-applied whenever it changes:
-- tables are created only if missing and no re-apply overwrites what an operator changed. A
-- hotspot that was edited or disabled keeps its values (seeds are skipped by name); one that was
-- deleted or renamed comes back on the next re-apply, so disable a seeded hotspot instead.
-- Chat lines are seeded only while a capital-battle activity has none at all, so edited or
-- deleted lines are never re-added.
-- Folded from the upstream mod-playerbots-pvp-life manual + updates scripts, none of which had
-- been applied through the updater.

CREATE TABLE IF NOT EXISTS `pvp_life_zone` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(64) NOT NULL,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `activity_type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=Skirmish,1=Duel,2=ForTheHorde,3=ForTheAlliance',
  `attacker_team` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0=Alliance,1=Horde,2=Any',
  `defender_team` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=Alliance,1=Horde,2=Any',
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 80,
  `map_id` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `rally_x` FLOAT NOT NULL DEFAULT 0,
  `rally_y` FLOAT NOT NULL DEFAULT 0,
  `rally_z` FLOAT NOT NULL DEFAULT 0,
  `rally_o` FLOAT NOT NULL DEFAULT 0,
  `target_x` FLOAT NOT NULL DEFAULT 0,
  `target_y` FLOAT NOT NULL DEFAULT 0,
  `target_z` FLOAT NOT NULL DEFAULT 0,
  `target_o` FLOAT NOT NULL DEFAULT 0,
  `attackers_min` SMALLINT UNSIGNED NOT NULL DEFAULT 3,
  `attackers_max` SMALLINT UNSIGNED NOT NULL DEFAULT 8,
  `defenders_min` SMALLINT UNSIGNED NOT NULL DEFAULT 3,
  `defenders_max` SMALLINT UNSIGNED NOT NULL DEFAULT 8,
  `duration_min` SMALLINT UNSIGNED NOT NULL DEFAULT 10 COMMENT 'minutes',
  `duration_max` SMALLINT UNSIGNED NOT NULL DEFAULT 25 COMMENT 'minutes',
  `weight` INT UNSIGNED NOT NULL DEFAULT 100,
  `cooldown_seconds` INT UNSIGNED NOT NULL DEFAULT 900,
  `last_start` INT UNSIGNED NOT NULL DEFAULT 0,
  `challenge_players` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `bot_chat` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_pvp_life_zone_name` (`name`),
  KEY `idx_pvp_life_zone_activity` (`enabled`,`activity_type`,`last_start`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `pvp_life_chat` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `activity_type` TINYINT UNSIGNED NOT NULL COMMENT '2=ForTheHorde,3=ForTheAlliance',
  `channel` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=Yell,1=World',
  `speaker_team` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '0=Alliance,1=Horde,2=Any',
  `text` VARCHAR(255) NOT NULL,
  `weight` INT UNSIGNED NOT NULL DEFAULT 100,
  PRIMARY KEY (`id`),
  KEY `idx_pvp_life_chat_pick` (`enabled`,`activity_type`,`channel`,`speaker_team`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Seed data for PvP Life zones (idempotent insert)
INSERT IGNORE INTO `pvp_life_zone`
(`name`,`enabled`,`activity_type`,`attacker_team`,`defender_team`,`min_level`,`max_level`,`map_id`,
 `rally_x`,`rally_y`,`rally_z`,`rally_o`,`target_x`,`target_y`,`target_z`,`target_o`,
 `attackers_min`,`attackers_max`,`defenders_min`,`defenders_max`,`duration_min`,`duration_max`,`weight`,`cooldown_seconds`,`last_start`,`challenge_players`,`bot_chat`)
VALUES
-- Duel zones: intentionally level 1-80. PvP Life runs all duels; City Life only places spectators at
-- these three spots. Goldshire is a busy town square, so it holds at most two pairs and does not
-- challenge real players (many there are new characters).
('StormwindDuel',1,1,0,0,1,80,0, -9122.49,350.862,93.7893,4.19046, -9122.49,350.862,93.7893,4.19046, 2,4,2,4,20,40,220,30,0,1,0),
('OrgrimmarDuel',1,1,1,1,1,80,1, 1282.04,-4395.88,26.3002,3.63561, 1282.04,-4395.88,26.3002,3.63561, 2,4,2,4,20,40,220,30,0,1,0),
('GoldshireDuel',1,1,0,0,1,80,0, -9456.63,80.21,56.50,1.25, -9456.63,80.21,56.50,1.25, 1,2,1,2,20,40,120,60,0,0,0),

-- Classic world-PvP hotspots (PvP Life baseline)
('STV_Nesingwary',1,0,1,0,25,50,0, -11490.5,-302.536,35.7362,3.57, -11490.5,-302.536,35.7362,3.57, 2,5,2,5,12,25,120,600,0,0,0),
('STV_Gurubashi',1,0,0,1,30,55,0, -13290.0,-274.0,20.0,0.0, -13245.0,-266.0,21.0,0.0, 2,5,2,5,12,25,130,600,0,0,0),
('Tanaris_Gadgetzan',1,0,1,0,40,65,1, -7200.0,-3860.0,9.0,0.0, -7165.0,-3805.0,9.0,0.0, 2,5,2,5,12,25,110,720,0,0,0),
('DarkPortal_Azeroth',1,0,0,1,55,80,0, -11820.0,-3200.0,-30.0,0.0, -11905.0,-3204.0,-14.0,0.0, 3,6,3,6,15,30,135,900,0,0,0),
('DarkPortal_Outland',1,0,1,0,58,80,530, -320.0,930.0,84.0,0.0, -248.0,922.0,84.0,0.0, 3,6,3,6,15,30,135,900,0,0,0),
('Shattrath_Outskirts',1,0,0,1,60,80,530, -1878.6,4927.39,-10.3383,4.33574, -1878.6,4927.39,-10.3383,4.33574, 2,5,2,5,12,25,100,900,0,0,0),
('K3_StormPeaks',1,0,1,0,70,80,571, 6197.19,-1001.47,408.121,4.65582, 6197.19,-1001.47,408.121,4.65582, 2,5,2,5,12,25,105,900,0,0,0),
('Wintergrasp',1,0,1,0,70,80,571, 4700.1,2867.58,387.683,3.3566, 4700.1,2867.58,387.683,3.3566, 3,6,3,6,12,25,115,900,0,0,0),
('Goldshire_Roaming',1,0,1,0,5,30,0, -9520.0,55.0,58.0,0.0, -9465.0,64.0,56.0,0.0, 2,3,1,3,8,18,85,720,0,0,0),
('Durotar_Roaming',1,0,0,1,5,30,1, 1375.0,-4550.0,24.0,0.0, 1450.0,-4418.0,25.0,0.0, 2,3,1,3,8,18,85,720,0,0,0),
('Orgrimmar_Zeppelin',0,0,0,1,10,80,1, 1320.0,-4630.0,25.0,0.0, 1360.0,-4590.0,25.0,0.0, 2,4,1,4,8,18,60,900,0,0,0),
('ForTheHorde_Stormwind',1,2,1,0,70,80,0, -9465.0,64.0,56.0,0.0, -8830.0,622.0,94.0,0.0, 3,7,3,7,15,25,100,3600,0,0,1),
('ForTheAlliance_Orgrimmar',1,3,0,1,70,80,1, 1915.0,-4755.0,40.0,0.0, 1500.0,-4415.0,22.0,0.0, 3,7,3,7,15,25,100,3600,0,0,1),

-- Imported World PvP Hotspots (Eastern Kingdoms & Kalimdor Campaigns)
('Lakeshire',1,0,1,0,20,35,0, -9325,-2285,70,0, -9215,-2150,70,0, 2,5,2,5, 5,12, 140, 1800, 0, 0, 0),
('Southshore',1,0,1,0,25,45,0, -760,-930,58,0, -845,-535,55,0, 3,7,3,7, 8,18, 180, 1500, 0, 0, 0),
('TarrenMill',1,0,0,1,25,45,0, -860,-560,55,0, -20,-915,57,0, 3,7,3,7, 8,18, 180, 1500, 0, 0, 0),
('Astranaar',1,0,1,0,20,35,1, 2300,-2500,110,0, 2750,-430,110,0, 2,5,2,5, 5,12, 120, 1800, 0, 0, 0),
('Splintertree',1,0,0,1,20,35,1, 2700,-500,110,0, 2300,-2500,110,0, 2,5,2,5, 5,12, 100, 1800, 0, 0, 0),
('Crossroads',1,0,0,1,20,35,1, -940,-3720,10,0, -456,-2650,95,0, 2,5,2,5, 5,12, 130, 1800, 0, 0, 0),
('RatchetRoad',1,0,1,0,20,35,1, -455,-2650,95,0, -955,-3740,8,0, 2,5,2,5, 5,12, 90, 1800, 0, 0, 0),
('SunRockRetreat',1,0,0,1,22,35,1, 1040,760,105,0, 960,915,105,0, 2,5,2,5, 5,12, 80, 2400, 0, 0, 0),
('FreewindPost',1,0,0,1,25,38,1, -5400,-2500,-50,0, -5440,-2440,90,0, 2,5,2,5, 5,12, 70, 2400, 0, 0, 0),
('RefugePointe',1,0,1,0,30,45,0, -920,-3490,70,0, -1260,-2520,35,0, 3,7,3,7, 8,18, 130, 1800, 0, 0, 0),
('Hammerfall',1,0,0,1,30,45,0, -1260,-2520,35,0, -920,-3490,70,0, 3,7,3,7, 8,18, 130, 1800, 0, 0, 0),
('NesingwaryCamp',1,0,1,0,30,45,0, -11670,-50,5,0, -11620,-70,10,0, 2,6,2,6, 6,14, 100, 1800, 0, 0, 0),
('BootyBayRoad',1,0,0,1,35,50,0, -14450,460,15,0, -14300,520,20,0, 2,6,2,6, 6,14, 80, 2400, 0, 0, 0),
('TheramoreRoad',1,0,1,0,35,45,1, -3600,-4500,10,0, -3700,-4400,15,0, 2,5,2,5, 6,14, 70, 2400, 0, 0, 0),
('Brackenwall',1,0,0,1,35,45,1, -3150,-2850,35,0, -2920,-3170,35,0, 2,5,2,5, 6,14, 60, 2400, 0, 0, 0),
('NijelsPoint',1,0,1,0,30,42,1, 280,-2380,105,0, 150,-1780,105,0, 2,5,2,5, 6,14, 60, 2400, 0, 0, 0),
('Shadowprey',1,0,0,1,30,42,1, -1650,3100,90,0, -1600,3050,90,0, 2,5,2,5, 6,14, 60, 2400, 0, 0, 0),
('Kargath',1,0,0,1,35,50,0, -6700,-2400,240,0, -6650,-2200,245,0, 2,5,2,5, 6,14, 70, 2400, 0, 0, 0),
('SearingGorge',0,0,2,2,45,55,0, -6500,-1200,180,0, -6550,-1150,180,0, 3,8,3,8, 8,18, 80, 2400, 0, 0, 0),
('BlackrockMountain',0,0,2,2,50,60,0, -7500,-1100,270,0, -7550,-1250,270,0, 4,10,4,10, 10,25, 100, 2400, 0, 0, 0),
('LightHopeChapel',0,0,1,0,55,60,0, 2250,-5300,85,0, 2280,-5310,85,0, 4,10,4,10, 10,25, 90, 3600, 0, 0, 0);

-- One-time migration: upstream shipped per-zone PvPLife.Zone.*.Min/MaxPopulation conf overrides for these
-- 15 hotspots, and a conf override beats the database (so .pvplife zone commands did nothing for them).
-- The overrides are now the seed values above; this moves a database seeded before that onto them too.
-- Each UPDATE only matches a row still holding the previous seed counts, so it cannot undo a GM edit.
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=4,`defenders_min`=2,`defenders_max`=4
  WHERE `name`='StormwindDuel' AND `attackers_min`=4 AND `attackers_max`=8 AND `defenders_min`=4 AND `defenders_max`=8;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=4,`defenders_min`=2,`defenders_max`=4
  WHERE `name`='OrgrimmarDuel' AND `attackers_min`=4 AND `attackers_max`=8 AND `defenders_min`=4 AND `defenders_max`=8;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=5,`defenders_min`=2,`defenders_max`=5
  WHERE `name`='STV_Nesingwary' AND `attackers_min`=5 AND `attackers_max`=10 AND `defenders_min`=5 AND `defenders_max`=10;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=5,`defenders_min`=2,`defenders_max`=5
  WHERE `name`='STV_Gurubashi' AND `attackers_min`=6 AND `attackers_max`=12 AND `defenders_min`=6 AND `defenders_max`=12;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=5,`defenders_min`=2,`defenders_max`=5
  WHERE `name`='Tanaris_Gadgetzan' AND `attackers_min`=5 AND `attackers_max`=10 AND `defenders_min`=5 AND `defenders_max`=10;
UPDATE `pvp_life_zone` SET `attackers_min`=3,`attackers_max`=6,`defenders_min`=3,`defenders_max`=6
  WHERE `name`='DarkPortal_Azeroth' AND `attackers_min`=6 AND `attackers_max`=14 AND `defenders_min`=6 AND `defenders_max`=14;
UPDATE `pvp_life_zone` SET `attackers_min`=3,`attackers_max`=6,`defenders_min`=3,`defenders_max`=6
  WHERE `name`='DarkPortal_Outland' AND `attackers_min`=6 AND `attackers_max`=14 AND `defenders_min`=6 AND `defenders_max`=14;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=5,`defenders_min`=2,`defenders_max`=5
  WHERE `name`='Shattrath_Outskirts' AND `attackers_min`=5 AND `attackers_max`=12 AND `defenders_min`=5 AND `defenders_max`=12;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=5,`defenders_min`=2,`defenders_max`=5
  WHERE `name`='K3_StormPeaks' AND `attackers_min`=5 AND `attackers_max`=12 AND `defenders_min`=5 AND `defenders_max`=12;
UPDATE `pvp_life_zone` SET `attackers_min`=3,`attackers_max`=6,`defenders_min`=3,`defenders_max`=6
  WHERE `name`='Wintergrasp' AND `attackers_min`=5 AND `attackers_max`=12 AND `defenders_min`=5 AND `defenders_max`=12;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=3,`defenders_min`=1,`defenders_max`=3
  WHERE `name`='Goldshire_Roaming' AND `attackers_min`=2 AND `attackers_max`=5 AND `defenders_min`=2 AND `defenders_max`=5;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=3,`defenders_min`=1,`defenders_max`=3
  WHERE `name`='Durotar_Roaming' AND `attackers_min`=2 AND `attackers_max`=5 AND `defenders_min`=2 AND `defenders_max`=5;
UPDATE `pvp_life_zone` SET `attackers_min`=2,`attackers_max`=4,`defenders_min`=1,`defenders_max`=4
  WHERE `name`='Orgrimmar_Zeppelin' AND `attackers_min`=2 AND `attackers_max`=6 AND `defenders_min`=2 AND `defenders_max`=6;
UPDATE `pvp_life_zone` SET `attackers_min`=3,`attackers_max`=7,`defenders_min`=3,`defenders_max`=7
  WHERE `name`='ForTheHorde_Stormwind' AND `attackers_min`=6 AND `attackers_max`=14 AND `defenders_min`=6 AND `defenders_max`=14;
UPDATE `pvp_life_zone` SET `attackers_min`=3,`attackers_max`=7,`defenders_min`=3,`defenders_max`=7
  WHERE `name`='ForTheAlliance_Orgrimmar' AND `attackers_min`=6 AND `attackers_max`=14 AND `defenders_min`=6 AND `defenders_max`=14;

-- Seed chatter for faction capital battles: only for an activity type that has no lines yet.
INSERT INTO `pvp_life_chat` (`enabled`,`activity_type`,`channel`,`speaker_team`,`text`,`weight`)
SELECT * FROM (
  -- For the Horde: Alliance defenders reacting.
  SELECT 1 AS enabled, 2 AS activity_type, 0 AS channel, 0 AS speaker_team, 'Horde in Stormwind!' AS text, 100 AS weight UNION ALL
  SELECT 1, 2, 0, 0, 'Horde at the gates!', 80 UNION ALL
  SELECT 1, 2, 0, 0, 'Defend Stormwind!', 80 UNION ALL
  SELECT 1, 2, 0, 0, 'Horde in SW!', 100 UNION ALL
  SELECT 1, 2, 1, 0, 'horde in sw', 120 UNION ALL
  SELECT 1, 2, 1, 0, 'horde at sw gates', 90 UNION ALL
  SELECT 1, 2, 1, 0, 'need help sw', 100 UNION ALL
  SELECT 1, 2, 1, 0, 'horde raid sw go def', 75 UNION ALL
  SELECT 1, 2, 1, 0, 'horde in stormwind', 90 UNION ALL
  -- For the Alliance: Horde defenders reacting.
  SELECT 1, 3, 0, 1, 'Alliance in Orgrimmar!', 100 UNION ALL
  SELECT 1, 3, 0, 1, 'Alliance at the gates!', 80 UNION ALL
  SELECT 1, 3, 0, 1, 'Defend Orgrimmar!', 80 UNION ALL
  SELECT 1, 3, 0, 1, 'Alliance in Org!', 100 UNION ALL
  SELECT 1, 3, 1, 1, 'ally in org', 120 UNION ALL
  SELECT 1, 3, 1, 1, 'alliance in org', 90 UNION ALL
  SELECT 1, 3, 1, 1, 'need help org', 100 UNION ALL
  SELECT 1, 3, 1, 1, 'ally raid org go def', 75 UNION ALL
  SELECT 1, 3, 1, 1, 'alliance at org gates', 90
) AS tmp
WHERE NOT EXISTS (
  SELECT 1 FROM `pvp_life_chat` c
  WHERE c.activity_type = tmp.activity_type
);

-- GM command help for the legacy .wpvp root. Only `wpvp` itself is a registered command (its
-- subcommands are parsed by the module), so the core skips any `wpvp ...` row with an error on
-- every start: remove the ones earlier versions of this file seeded.
DELETE FROM `command` WHERE `name` IN ('wpvp help', 'wpvp status', 'wpvp scan', 'wpvp start', 'wpvp stop',
  'wpvp spot', 'wpvp spot list', 'wpvp spot create', 'wpvp spot rally', 'wpvp spot target',
  'wpvp spot counts', 'wpvp spot duration', 'wpvp spot enable');
INSERT IGNORE INTO `command` (`name`, `security`, `help`) VALUES
('wpvp', 3, 'Syntax: .wpvp <subcommand>\nLegacy alias of .pvplife (spot = zone). .wpvp help lists the subcommands.');
