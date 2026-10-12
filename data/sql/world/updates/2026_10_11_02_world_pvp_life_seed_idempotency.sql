-- Non-destructive seed insertion and command registration for mod-playerbots-pvp-life
-- Inserts missing baseline hotspots and wpvp compatibility commands without overwriting admin customizations.

INSERT IGNORE INTO `pvp_life_zone`
(`name`,`enabled`,`activity_type`,`attacker_team`,`defender_team`,`min_level`,`max_level`,`map_id`,
 `rally_x`,`rally_y`,`rally_z`,`rally_o`,`target_x`,`target_y`,`target_z`,`target_o`,
 `attackers_min`,`attackers_max`,`defenders_min`,`defenders_max`,`duration_min`,`duration_max`,`weight`,`cooldown_seconds`,`last_start`,`challenge_players`,`bot_chat`)
VALUES
('StormwindDuel',1,1,0,0,1,80,0, -9122.49,350.862,93.7893,4.19046, -9122.49,350.862,93.7893,4.19046, 4,8,4,8,20,40,220,30,0,1,0),
('OrgrimmarDuel',1,1,1,1,1,80,1, 1282.04,-4395.88,26.3002,3.63561, 1282.04,-4395.88,26.3002,3.63561, 4,8,4,8,20,40,220,30,0,1,0),
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
