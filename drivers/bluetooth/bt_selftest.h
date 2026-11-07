/* bt_selftest.h — the BT transport, driven by a scripted controller
 * (RESIDUE3 C6).
 *
 * WHY.  tests/unit/test_bt_transport.c proves the same code on the host.  What
 * it cannot prove is that the KERNEL build reaches those paths — the driver is
 * compiled twice, and a build-specific difference (a different include, a
 * different integer width on another architecture) would not show up there.
 * It also cannot show the receipt lines a boot log carries.
 *
 * QEMU supplies no Bluetooth controller at all (its hw/bt subsystem was
 * removed in 9.0), so the controller here is bytes: the same scripted
 * CSR-class responder the host test uses, with the answers HCI Core 5.x
 * specifies.  The receipts quote the controller's own BD_ADDR and inquiry
 * addresses, so a reader can tell a real round trip from a printf.
 */
#ifndef AURALITE_DRIVERS_BLUETOOTH_BT_SELFTEST_H
#define AURALITE_DRIVERS_BLUETOOTH_BT_SELFTEST_H

/* Run the bring-up + inquiry + pairing-refusal flow against the scripted
 * controller and print PASS/FAIL lines.  Never touches a device. */
void bt_selftest_run(void);

#endif /* AURALITE_DRIVERS_BLUETOOTH_BT_SELFTEST_H */
