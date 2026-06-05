-- Requires pgcrypto for SHA-256 password hashes (same hex format as MySQL SHA2(..., 256))
CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE smpp_queued_pdu (
  global_id BIGSERIAL PRIMARY KEY,
  system_id VARCHAR(64) NOT NULL,
  time BIGINT,
  pdu BYTEA
);
CREATE INDEX IF NOT EXISTS smpp_queued_pdu_system_id_idx ON smpp_queued_pdu (system_id);

CREATE TABLE smpp_route (
  route_id BIGSERIAL PRIMARY KEY,
  direction INT NOT NULL,
  regex TEXT,
  cost DOUBLE PRECISION,
  system_id VARCHAR(64),
  smsc_id VARCHAR(64),
  source_regex TEXT,
  priority INT DEFAULT 0
);
CREATE INDEX IF NOT EXISTS smpp_route_direction_idx ON smpp_route (direction);
CREATE INDEX IF NOT EXISTS smpp_route_system_id_idx ON smpp_route (system_id);
CREATE INDEX IF NOT EXISTS smpp_route_smsc_id_idx ON smpp_route (smsc_id);

CREATE TABLE smpp_store (
  global_id BIGSERIAL PRIMARY KEY,
  sender TEXT,
  receiver TEXT,
  udhdata TEXT,
  msgdata TEXT,
  time BIGINT NOT NULL,
  smsc_id TEXT,
  smsc_number TEXT,
  foreign_id TEXT,
  service TEXT,
  account TEXT,
  id VARCHAR(128),
  sms_type BIGINT NOT NULL,
  mclass BIGINT NOT NULL,
  mwi BIGINT NOT NULL,
  coding BIGINT NOT NULL,
  compress BIGINT NOT NULL,
  validity BIGINT NOT NULL,
  deferred BIGINT NOT NULL,
  dlr_mask BIGINT NOT NULL,
  dlr_url TEXT,
  pid BIGINT NOT NULL,
  alt_dcs BIGINT NOT NULL,
  rpi BIGINT NOT NULL,
  charset TEXT,
  boxc_id TEXT,
  binfo TEXT,
  msg_left BIGINT NOT NULL,
  priority BIGINT NOT NULL,
  resend_try BIGINT NOT NULL,
  resend_time BIGINT NOT NULL,
  meta_data TEXT
);
CREATE INDEX IF NOT EXISTS smpp_store_service_idx ON smpp_store (service);
CREATE INDEX IF NOT EXISTS smpp_store_sms_type_idx ON smpp_store (sms_type);

-- Bearerbox/SMSC fallback queue (database-queue-store-table)
CREATE TABLE smpp_store_queue (
  global_id BIGSERIAL PRIMARY KEY,
  sender TEXT,
  receiver TEXT,
  udhdata TEXT,
  msgdata TEXT,
  time BIGINT NOT NULL,
  smsc_id TEXT,
  smsc_number TEXT,
  foreign_id TEXT,
  service TEXT,
  account TEXT,
  id VARCHAR(128),
  sms_type BIGINT NOT NULL,
  mclass BIGINT NOT NULL,
  mwi BIGINT NOT NULL,
  coding BIGINT NOT NULL,
  compress BIGINT NOT NULL,
  validity BIGINT NOT NULL,
  deferred BIGINT NOT NULL,
  dlr_mask BIGINT NOT NULL,
  dlr_url TEXT,
  pid BIGINT NOT NULL,
  alt_dcs BIGINT NOT NULL,
  rpi BIGINT NOT NULL,
  charset TEXT,
  boxc_id TEXT,
  binfo TEXT,
  msg_left BIGINT NOT NULL,
  priority BIGINT NOT NULL,
  resend_try BIGINT NOT NULL,
  resend_time BIGINT NOT NULL,
  meta_data TEXT
);
CREATE INDEX IF NOT EXISTS smpp_store_queue_service_idx ON smpp_store_queue (service);
CREATE INDEX IF NOT EXISTS smpp_store_queue_sms_type_idx ON smpp_store_queue (sms_type);

CREATE TABLE smpp_dlr (
  global_id BIGSERIAL PRIMARY KEY,
  message_id VARCHAR(128) NOT NULL,
  service VARCHAR(64) NOT NULL,
  status VARCHAR(16) DEFAULT 'DELIVRD',
  err_code INT DEFAULT 0,
  submit_date VARCHAR(20),
  done_date VARCHAR(20),
  destination_addr VARCHAR(32),
  source_addr VARCHAR(32),
  smsc_id VARCHAR(64),
  text TEXT,
  processed SMALLINT DEFAULT 0
);
CREATE INDEX IF NOT EXISTS smpp_dlr_service_processed_idx ON smpp_dlr (service, processed);
CREATE INDEX IF NOT EXISTS smpp_dlr_message_id_idx ON smpp_dlr (message_id);

CREATE TABLE smpp_user (
  system_id VARCHAR(15) NOT NULL PRIMARY KEY,
  password VARCHAR(64) NOT NULL,
  throughput DOUBLE PRECISION NOT NULL DEFAULT 0,
  default_smsc VARCHAR(64),
  default_cost DOUBLE PRECISION NOT NULL,
  enable_prepaid_billing INT NOT NULL DEFAULT 0,
  credit DOUBLE PRECISION NOT NULL DEFAULT 0,
  callback_url VARCHAR(255),
  simulate SMALLINT NOT NULL DEFAULT 0,
  simulate_dlr_fail SMALLINT NOT NULL DEFAULT 0,
  simulate_deliver_every INT NOT NULL,
  simulate_permanent_failure_every INT NOT NULL,
  simulate_temporary_failure_every INT NOT NULL,
  simulate_mo_every INT NOT NULL,
  max_binds INT NOT NULL DEFAULT 0,
  connect_allow_ip TEXT
);

-- Example user (password = demopass):
-- INSERT INTO smpp_user (system_id, password, throughput, default_cost, simulate_deliver_every, simulate_permanent_failure_every, simulate_temporary_failure_every, simulate_mo_every)
-- VALUES ('demouser', encode(digest('demopass', 'sha256'), 'hex'), 10.0, 1.0, 0, 0, 0, 0);

CREATE TABLE smpp_version (
  component VARCHAR(54) NOT NULL PRIMARY KEY,
  version INT NOT NULL
);
