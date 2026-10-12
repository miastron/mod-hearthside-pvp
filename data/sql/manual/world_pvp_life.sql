-- PvP Life
-- Apply to the AzerothCore WORLD database.

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
-- Duel zones: intentionally level 1-80. Both modules run capital gate duels concurrently.
('StormwindDuel',1,1,0,0,1,80,0, -9122.49,350.862,93.7893,4.19046, -9122.49,350.862,93.7893,4.19046, 4,8,4,8,20,40,220,30,0,1,0),
('OrgrimmarDuel',1,1,1,1,1,80,1, 1282.04,-4395.88,26.3002,3.63561, 1282.04,-4395.88,26.3002,3.63561, 4,8,4,8,20,40,220,30,0,1,0),

-- Classic world-PvP hotspots (PvP Life baseline)
('STV_Nesingwary',1,0,1,0,25,50,0, -11490.5,-302.536,35.7362,3.57, -11490.5,-302.536,35.7362,3.57, 5,10,5,10,12,25,120,600,0,0,0),
('STV_Gurubashi',1,0,0,1,30,55,0, -13290.0,-274.0,20.0,0.0, -13245.0,-266.0,21.0,0.0, 6,12,6,12,12,25,130,600,0,0,0),
('Tanaris_Gadgetzan',1,0,1,0,40,65,1, -7200.0,-3860.0,9.0,0.0, -7165.0,-3805.0,9.0,0.0, 5,10,5,10,12,25,110,720,0,0,0),
('DarkPortal_Azeroth',1,0,0,1,55,80,0, -11820.0,-3200.0,-30.0,0.0, -11905.0,-3204.0,-14.0,0.0, 6,14,6,14,15,30,135,900,0,0,0),
('DarkPortal_Outland',1,0,1,0,58,80,530, -320.0,930.0,84.0,0.0, -248.0,922.0,84.0,0.0, 6,14,6,14,15,30,135,900,0,0,0),
('Shattrath_Outskirts',1,0,0,1,60,80,530, -1878.6,4927.39,-10.3383,4.33574, -1878.6,4927.39,-10.3383,4.33574, 5,12,5,12,12,25,100,900,0,0,0),
('K3_StormPeaks',1,0,1,0,70,80,571, 6197.19,-1001.47,408.121,4.65582, 6197.19,-1001.47,408.121,4.65582, 5,12,5,12,12,25,105,900,0,0,0),
('Wintergrasp',1,0,1,0,70,80,571, 4700.1,2867.58,387.683,3.3566, 4700.1,2867.58,387.683,3.3566, 5,12,5,12,12,25,115,900,0,0,0),
('Goldshire_Roaming',1,0,1,0,5,30,0, -9520.0,55.0,58.0,0.0, -9465.0,64.0,56.0,0.0, 2,5,2,5,8,18,85,720,0,0,0),
('Durotar_Roaming',1,0,0,1,5,30,1, 1375.0,-4550.0,24.0,0.0, 1450.0,-4418.0,25.0,0.0, 2,5,2,5,8,18,85,720,0,0,0),
('Orgrimmar_Zeppelin',0,0,0,1,10,80,1, 1320.0,-4630.0,25.0,0.0, 1360.0,-4590.0,25.0,0.0, 2,6,2,6,8,18,60,900,0,0,0),
('ForTheHorde_Stormwind',1,2,1,0,70,80,0, -9465.0,64.0,56.0,0.0, -8830.0,622.0,94.0,0.0, 6,14,6,14,15,25,100,3600,0,0,1),
('ForTheAlliance_Orgrimmar',1,3,0,1,70,80,1, 1915.0,-4755.0,40.0,0.0, 1500.0,-4415.0,22.0,0.0, 6,14,6,14,15,25,100,3600,0,0,1),

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

-- Seed chatter for faction capital battles (non-destructive insert if absent)
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
    AND c.channel = tmp.channel
    AND c.speaker_team = tmp.speaker_team
    AND c.text = tmp.text
);

-- Compatibility GM Commands for .wpvp (non-destructive insert if absent)
INSERT IGNORE INTO `command` (`name`, `security`, `help`) VALUES
('wpvp', 3, 'Syntax: .wpvp help\nPlayerbot World PvP GM commands (PvP Life Compatibility).'),
('wpvp help', 3, 'Syntax: .wpvp help\nShows Playerbot World PvP commands.'),
('wpvp status', 3, 'Syntax: .wpvp status\nShows active world-PvP events.'),
('wpvp scan', 3, 'Syntax: .wpvp scan [minLvl] [maxLvl]\nShows eligible online random Playerbots and per-hotspot readiness.'),
('wpvp start', 3, 'Syntax: .wpvp start <spot>\nForce-start a configured world-PvP hotspot.'),
('wpvp stop', 3, 'Syntax: .wpvp stop <id|all>\nStop one or all active world-PvP events.'),
('wpvp spot', 3, 'Syntax: .wpvp spot <subcommand>\nManage Playerbot World PvP hotspots.'),
('wpvp spot list', 3, 'Syntax: .wpvp spot list\nList configured hotspots.'),
('wpvp spot create', 3, 'Syntax: .wpvp spot create <name> <attacker> <defender> <minLvl> <maxLvl>\nCreate/update a hotspot at your current position.'),
('wpvp spot rally', 3, 'Syntax: .wpvp spot rally <name>\nSave your current location as attacker rally/outside-town point.'),
('wpvp spot target', 3, 'Syntax: .wpvp spot target <name>\nSave your current location as target/defender point.'),
('wpvp spot counts', 3, 'Syntax: .wpvp spot counts <name> <attMin> <attMax> <defMin> <defMax>\nSet event population.'),
('wpvp spot duration', 3, 'Syntax: .wpvp spot duration <name> <minMinutes> <maxMinutes>\nSet event duration.'),
('wpvp spot enable', 3, 'Syntax: .wpvp spot enable <name> <0|1>\nEnable or disable a hotspot.');
