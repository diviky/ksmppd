-- Per-ESME database-store-primary (optional upgrade for existing installs).
-- NULL = inherit ksmppd.conf database-store-primary; 0 = bearerbox; 1 = DB primary MT.

ALTER TABLE `smpp_user`
  ADD COLUMN `database_store_primary` tinyint(1) DEFAULT NULL
  COMMENT 'NULL=inherit ksmppd database-store-primary'
  AFTER `connect_allow_ip`;
