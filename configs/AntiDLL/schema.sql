-- 1sT-AntiDLL MySQL schema
-- Run this once on your MySQL/MariaDB server to create the required tables.

CREATE TABLE IF NOT EXISTS antidll_servers (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    server_name VARCHAR(128) NOT NULL,
    server_group VARCHAR(64) DEFAULT NULL,
    region      VARCHAR(64) DEFAULT NULL,
    ip          VARCHAR(64) NOT NULL,
    port        INT NOT NULL,
    enabled     TINYINT(1) DEFAULT 1,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uniq_ip_port (ip, port)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS antidll_detections (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    server_name         VARCHAR(128),
    server_group        VARCHAR(64),
    region              VARCHAR(64),
    server_ip           VARCHAR(64),
    server_port         INT,
    map_name            VARCHAR(128),
    player_name         VARCHAR(128),
    steamid64           VARCHAR(32),
    detection_category  VARCHAR(64),
    detection_reason    VARCHAR(255),
    points_added        INT,
    points_total        INT,
    threshold           INT,
    action_taken        VARCHAR(64),
    evidence_json       JSON
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Example: register your servers
-- INSERT INTO antidll_servers (server_name, server_group, region, ip, port, enabled)
-- VALUES
-- ('1sT Competitive #12', 'Competitive', 'MN', '103.236.xxx.xxx', 27027, 1),
-- ('1sT Competitive #13', 'Competitive', 'MN', '103.236.xxx.xxx', 27028, 1);
