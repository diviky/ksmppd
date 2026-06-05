-- Test users for Docker (MySQL 8: SHA2 instead of deprecated PASSWORD())

INSERT INTO `smpp_user`
    (`system_id`, `password`, `throughput`, `default_smsc`, `default_cost`, `enable_prepaid_billing`, `credit`,
    `callback_url`, `simulate`, `simulate_dlr_fail`, `simulate_deliver_every`, `simulate_permanent_failure_every`, `simulate_temporary_failure_every`,
    `simulate_mo_every`, `max_binds`)
VALUES
    ('smppuserA', SHA2('userA', 256), 11.00000, 'ksmppd2', 0, 1, 0, NULL, 0, 0, 0, 0, 0, 0, 0),
    ('smppuserB', SHA2('userB', 256), 0.00000, NULL, 0, 0, 0, NULL, 1, 0, 1, 0, 0, 0, 2),
    ('smppuserC', SHA2('userC', 256), 0.00000, NULL, 0, 0, 0, NULL, 1, 0, 1, 0, 0, 2, 0);
