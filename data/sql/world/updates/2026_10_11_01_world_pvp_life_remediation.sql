-- Auto-update migration: create pvp_life_chat if missing and seed baseline capital defense chatter
-- Fully idempotent and non-destructive

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
